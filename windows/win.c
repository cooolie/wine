/*
 * Window related functions
 *
 * Copyright 1993, 1994 Alexandre Julliard
 */

#include <stdlib.h>
#include <string.h>
#include "windef.h"
#include "wine/winbase16.h"
#include "wine/winuser16.h"
#include "wine/server.h"
#include "wine/unicode.h"
#include "win.h"
#include "heap.h"
#include "user.h"
#include "dce.h"
#include "controls.h"
#include "cursoricon.h"
#include "hook.h"
#include "message.h"
#include "queue.h"
#include "task.h"
#include "winpos.h"
#include "winerror.h"
#include "stackframe.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(win);
DECLARE_DEBUG_CHANNEL(msg);

/**********************************************************************/

/* Desktop window */
static WND *pWndDesktop = NULL;

static WORD wDragWidth = 4;
static WORD wDragHeight= 3;

static void *user_handles[65536];

/* thread safeness */
extern SYSLEVEL USER_SysLevel;  /* FIXME */

/***********************************************************************
 *           WIN_SuspendWndsLock
 *
 *   Suspend the lock on WND structures.
 *   Returns the number of locks suspended
 */
int WIN_SuspendWndsLock( void )
{
    int isuspendedLocks = _ConfirmSysLevel( &USER_SysLevel );
    int count = isuspendedLocks;

    while ( count-- > 0 )
        _LeaveSysLevel( &USER_SysLevel );

    return isuspendedLocks;
}

/***********************************************************************
 *           WIN_RestoreWndsLock
 *
 *  Restore the suspended locks on WND structures
 */
void WIN_RestoreWndsLock( int ipreviousLocks )
{
    while ( ipreviousLocks-- > 0 )
        _EnterSysLevel( &USER_SysLevel );
}

/***********************************************************************
 *           create_window_handle
 *
 * Create a window handle with the server.
 */
static WND *create_window_handle( HWND parent, HWND owner, INT size )
{
    BOOL res;
    user_handle_t handle = 0;
    WND *win = HeapAlloc( GetProcessHeap(), 0, size );

    if (!win) return NULL;

    USER_Lock();

    SERVER_START_REQ( create_window )
    {
        req->parent = parent;
        req->owner = owner;
        if ((res = !SERVER_CALL_ERR())) handle = req->handle;
    }
    SERVER_END_REQ;

    if (!res)
    {
        USER_Unlock();
        HeapFree( GetProcessHeap(), 0, win );
        return NULL;
    }
    user_handles[LOWORD(handle)] = win;
    win->hwndSelf = handle;
    win->dwMagic = WND_MAGIC;
    win->irefCount = 1;
    return win;
}


/***********************************************************************
 *           free_window_handle
 *
 * Free a window handle.
 */
static WND *free_window_handle( HWND hwnd )
{
    WND *ptr;

    USER_Lock();
    if ((ptr = user_handles[LOWORD(hwnd)]))
    {
        SERVER_START_REQ( destroy_window )
        {
            req->handle = hwnd;
            if (!SERVER_CALL_ERR())
                user_handles[LOWORD(hwnd)] = NULL;
            else
                ptr = NULL;
        }
        SERVER_END_REQ;
    }
    USER_Unlock();
    if (ptr) HeapFree( GetProcessHeap(), 0, ptr );
    return ptr;
}


/***********************************************************************
 *           get_wnd_ptr
 *
 * Return a pointer to the WND structure if local to the process.
 * If ret value is non-NULL, the user lock is held.
 */
static WND *get_wnd_ptr( HWND hwnd )
{
    WND * ptr;

    if (!hwnd) return NULL;

    USER_Lock();
    if ((ptr = user_handles[LOWORD(hwnd)]))
    {
        if (ptr->dwMagic == WND_MAGIC && (!HIWORD(hwnd) || hwnd == ptr->hwndSelf))
            return ptr;
    }
    USER_Unlock();
    return NULL;
}


/***********************************************************************
 *           WIN_Handle32
 *
 * Convert a 16-bit window handle to a full 32-bit handle.
 */
HWND WIN_Handle32( HWND16 hwnd16 )
{
    WND *ptr;
    HWND hwnd = (HWND)(ULONG_PTR)hwnd16;

    if (hwnd16 <= 1 || hwnd16 == 0xffff) return hwnd;
    /* do sign extension for -2 and -3 */
    if (hwnd16 >= (HWND16)-3) return (HWND)(LONG_PTR)(INT16)hwnd16;

    if ((ptr = get_wnd_ptr( hwnd )))
    {
        hwnd = ptr->hwndSelf;
        USER_Unlock();
    }
    else  /* may belong to another process */
    {
        SERVER_START_REQ( get_window_info )
        {
            req->handle = hwnd;
            if (!SERVER_CALL_ERR()) hwnd = req->full_handle;
        }
        SERVER_END_REQ;
    }
    return hwnd;
}


/***********************************************************************
 *           WIN_FindWndPtr
 *
 * Return a pointer to the WND structure corresponding to a HWND.
 */
WND * WIN_FindWndPtr( HWND hwnd )
{
    WND * ptr;

    if (!hwnd) return NULL;

    if ((ptr = get_wnd_ptr( hwnd )))
    {
        /* increment destruction monitoring */
        ptr->irefCount++;
        return ptr;
    }

    /* check other processes */
    if (IsWindow( hwnd ))
    {
        ERR( "window %04x belongs to other process\n", hwnd );
        /* DbgBreakPoint(); */
    }
    SetLastError( ERROR_INVALID_WINDOW_HANDLE );
    return NULL;
}


/***********************************************************************
 *           WIN_LockWndPtr
 *
 * Use in case the wnd ptr is not initialized with WIN_FindWndPtr
 * but by initWndPtr;
 * Returns the locked initialisation pointer
 */
WND *WIN_LockWndPtr(WND *initWndPtr)
{
    if(!initWndPtr) return 0;

    /* Lock all WND structures for thread safeness*/
    USER_Lock();
    /*and increment destruction monitoring*/
    initWndPtr->irefCount++;

    return initWndPtr;

}

/***********************************************************************
 *           WIN_ReleaseWndPtr
 *
 * Release the pointer to the WND structure.
 */
void WIN_ReleaseWndPtr(WND *wndPtr)
{
    if(!wndPtr) return;

    /*Decrement destruction monitoring value*/
     wndPtr->irefCount--;
     /* Check if it's time to release the memory*/
     if(wndPtr->irefCount == 0 && !wndPtr->dwMagic)
     {
         /* Release memory */
         free_window_handle( wndPtr->hwndSelf );
     }
     else if(wndPtr->irefCount < 0)
     {
         /* This else if is useful to monitor the WIN_ReleaseWndPtr function */
         ERR("forgot a Lock on %p somewhere\n",wndPtr);
     }
     /*unlock all WND structures for thread safeness*/
     USER_Unlock();
}

/***********************************************************************
 *           WIN_UpdateWndPtr
 *
 * Updates the value of oldPtr to newPtr.
 */
void WIN_UpdateWndPtr(WND **oldPtr, WND *newPtr)
{
    WND *tmpWnd = NULL;

    tmpWnd = WIN_LockWndPtr(newPtr);
    WIN_ReleaseWndPtr(*oldPtr);
    *oldPtr = tmpWnd;

}


/***********************************************************************
 *           WIN_UnlinkWindow
 *
 * Remove a window from the siblings linked list.
 */
void WIN_UnlinkWindow( HWND hwnd )
{
    WIN_LinkWindow( hwnd, 0, 0 );
}


/***********************************************************************
 *           WIN_LinkWindow
 *
 * Insert a window into the siblings linked list.
 * The window is inserted after the specified window, which can also
 * be specified as HWND_TOP or HWND_BOTTOM.
 * If parent is 0, window is unlinked from the tree.
 */
void WIN_LinkWindow( HWND hwnd, HWND parent, HWND hwndInsertAfter )
{
    WND *wndPtr, **ppWnd, *parentPtr = NULL;
    BOOL ret;

    if (!(wndPtr = WIN_FindWndPtr( hwnd ))) return;
    if (parent && !(parentPtr = WIN_FindWndPtr( parent )))
    {
        WIN_ReleaseWndPtr(wndPtr);
        return;
    }

    SERVER_START_REQ( link_window )
    {
        req->handle   = hwnd;
        req->parent   = parent;
        req->previous = hwndInsertAfter;
        ret = !SERVER_CALL_ERR();
    }
    SERVER_END_REQ;
    if (!ret) goto done;

    /* first unlink it if it is linked */
    if (wndPtr->parent)
    {
        ppWnd = &wndPtr->parent->child;
        while (*ppWnd && *ppWnd != wndPtr) ppWnd = &(*ppWnd)->next;
        if (*ppWnd) *ppWnd = wndPtr->next;
    }

    if (parentPtr)
    {
        wndPtr->parent = parentPtr;
        if ((hwndInsertAfter == HWND_TOP) || (hwndInsertAfter == HWND_BOTTOM))
        {
            ppWnd = &parentPtr->child;  /* Point to first sibling hwnd */
            if (hwndInsertAfter == HWND_BOTTOM)  /* Find last sibling hwnd */
                while (*ppWnd) ppWnd = &(*ppWnd)->next;
        }
        else  /* Normal case */
        {
            WND * afterPtr = WIN_FindWndPtr( hwndInsertAfter );
            if (!afterPtr) goto done;
            ppWnd = &afterPtr->next;
            WIN_ReleaseWndPtr(afterPtr);
        }
        wndPtr->next = *ppWnd;
        *ppWnd = wndPtr;
    }
    else wndPtr->next = NULL;  /* unlinked */

 done:
    WIN_ReleaseWndPtr( parentPtr );
    WIN_ReleaseWndPtr( wndPtr );
}


/***********************************************************************
 *           WIN_FindWinToRepaint
 *
 * Find a window that needs repaint.
 */
HWND WIN_FindWinToRepaint( HWND hwnd )
{
    HWND hwndRet;
    WND *pWnd;

    /* Note: the desktop window never gets WM_PAINT messages
     * The real reason why is because Windows DesktopWndProc
     * does ValidateRgn inside WM_ERASEBKGND handler.
     */
    if (hwnd == GetDesktopWindow()) hwnd = 0;

    pWnd = hwnd ? WIN_FindWndPtr(hwnd) : WIN_LockWndPtr(pWndDesktop->child);

    for ( ; pWnd ; WIN_UpdateWndPtr(&pWnd,pWnd->next))
    {
        if (!(pWnd->dwStyle & WS_VISIBLE)) continue;
        if ((pWnd->hrgnUpdate || (pWnd->flags & WIN_INTERNAL_PAINT)) &&
            GetWindowThreadProcessId( pWnd->hwndSelf, NULL ) == GetCurrentThreadId())
            break;
        if (pWnd->child )
        {
            if ((hwndRet = WIN_FindWinToRepaint( pWnd->child->hwndSelf )) )
            {
                WIN_ReleaseWndPtr(pWnd);
                return hwndRet;
            }
        }
    }

    if(!pWnd)
    {
        TRACE("nothing found\n");
        return 0;
    }
    hwndRet = pWnd->hwndSelf;

    /* look among siblings if we got a transparent window */
    while (pWnd)
    {
        if (!(pWnd->dwExStyle & WS_EX_TRANSPARENT) &&
            (pWnd->hrgnUpdate || (pWnd->flags & WIN_INTERNAL_PAINT)) &&
            GetWindowThreadProcessId( pWnd->hwndSelf, NULL ) == GetCurrentThreadId())
        {
            hwndRet = pWnd->hwndSelf;
            WIN_ReleaseWndPtr(pWnd);
            break;
        }
        WIN_UpdateWndPtr(&pWnd,pWnd->next);
    }
    TRACE("found %04x\n",hwndRet);
    return hwndRet;
}


/***********************************************************************
 *           WIN_DestroyWindow
 *
 * Destroy storage associated to a window. "Internals" p.358
 * returns a locked wndPtr->next
 */
static WND* WIN_DestroyWindow( WND* wndPtr )
{
    HWND hwnd = wndPtr->hwndSelf;
    WND *pWnd;

    TRACE("%04x\n", wndPtr->hwndSelf );

    /* free child windows */
    WIN_LockWndPtr(wndPtr->child);
    while ((pWnd = wndPtr->child))
    {
        wndPtr->child = WIN_DestroyWindow( pWnd );
        WIN_ReleaseWndPtr(pWnd);
    }

    /*
     * Clear the update region to make sure no WM_PAINT messages will be
     * generated for this window while processing the WM_NCDESTROY.
     */
    RedrawWindow( wndPtr->hwndSelf, NULL, 0,
                  RDW_VALIDATE | RDW_NOFRAME | RDW_NOERASE | RDW_NOINTERNALPAINT | RDW_NOCHILDREN);

    /*
     * Send the WM_NCDESTROY to the window being destroyed.
     */
    SendMessageA( wndPtr->hwndSelf, WM_NCDESTROY, 0, 0);

    /* FIXME: do we need to fake QS_MOUSEMOVE wakebit? */

    WINPOS_CheckInternalPos( hwnd );
    if( hwnd == GetCapture()) ReleaseCapture();

    /* free resources associated with the window */

    TIMER_RemoveWindowTimers( wndPtr->hwndSelf );
    PROPERTY_RemoveWindowProps( wndPtr );

    /* toss stale messages from the queue */

    QUEUE_CleanupWindow( hwnd );
    wndPtr->hmemTaskQ = 0;

    if (!(wndPtr->dwStyle & WS_CHILD))
       if (wndPtr->wIDmenu)
       {
	   DestroyMenu( wndPtr->wIDmenu );
	   wndPtr->wIDmenu = 0;
       }
    if (wndPtr->hSysMenu)
    {
	DestroyMenu( wndPtr->hSysMenu );
	wndPtr->hSysMenu = 0;
    }
    USER_Driver.pDestroyWindow( wndPtr->hwndSelf );
    DCE_FreeWindowDCE( wndPtr->hwndSelf );    /* Always do this to catch orphaned DCs */
    WINPROC_FreeProc( wndPtr->winproc, WIN_PROC_WINDOW );
    CLASS_RemoveWindow( wndPtr->class );
    wndPtr->class = NULL;
    wndPtr->dwMagic = 0;  /* Mark it as invalid */

    WIN_UpdateWndPtr(&pWnd,wndPtr->next);

    return pWnd;
}

/***********************************************************************
 *           WIN_DestroyThreadWindows
 *
 * Destroy all children of 'wnd' owned by the current thread.
 * Return TRUE if something was done.
 */
void WIN_DestroyThreadWindows( HWND hwnd )
{
    HWND *list;
    int i;

    if (!(list = WIN_ListChildren( hwnd ))) return;
    for (i = 0; list[i]; i++)
    {
        if (!IsWindow( list[i] )) continue;
        if (GetWindowThreadProcessId( list[i], NULL ) == GetCurrentThreadId())
            DestroyWindow( list[i] );
        else
            WIN_DestroyThreadWindows( list[i] );
    }
    HeapFree( GetProcessHeap(), 0, list );
}

/***********************************************************************
 *           WIN_CreateDesktopWindow
 *
 * Create the desktop window.
 */
BOOL WIN_CreateDesktopWindow(void)
{
    struct tagCLASS *class;
    HWND hwndDesktop;
    INT wndExtra;
    DWORD clsStyle;
    WNDPROC winproc;
    DCE *dce;
    CREATESTRUCTA cs;

    TRACE("Creating desktop window\n");


    if (!WINPOS_CreateInternalPosAtom() ||
        !(class = CLASS_AddWindow( (ATOM)LOWORD(DESKTOP_CLASS_ATOM), 0, WIN_PROC_32W,
                                   &wndExtra, &winproc, &clsStyle, &dce )))
        return FALSE;

    pWndDesktop = create_window_handle( 0, 0, sizeof(WND) + wndExtra );
    if (!pWndDesktop) return FALSE;
    hwndDesktop = pWndDesktop->hwndSelf;

    pWndDesktop->tid               = 0;  /* nobody owns the desktop */
    pWndDesktop->next              = NULL;
    pWndDesktop->child             = NULL;
    pWndDesktop->parent            = NULL;
    pWndDesktop->owner             = 0;
    pWndDesktop->class             = class;
    pWndDesktop->hInstance         = 0;
    pWndDesktop->rectWindow.left   = 0;
    pWndDesktop->rectWindow.top    = 0;
    pWndDesktop->rectWindow.right  = GetSystemMetrics(SM_CXSCREEN);
    pWndDesktop->rectWindow.bottom = GetSystemMetrics(SM_CYSCREEN);
    pWndDesktop->rectClient        = pWndDesktop->rectWindow;
    pWndDesktop->text              = NULL;
    pWndDesktop->hmemTaskQ         = 0;
    pWndDesktop->hrgnUpdate        = 0;
    pWndDesktop->hwndLastActive    = hwndDesktop;
    pWndDesktop->dwStyle           = WS_VISIBLE | WS_CLIPCHILDREN |
                                     WS_CLIPSIBLINGS;
    pWndDesktop->dwExStyle         = 0;
    pWndDesktop->clsStyle          = clsStyle;
    pWndDesktop->dce               = NULL;
    pWndDesktop->pVScroll          = NULL;
    pWndDesktop->pHScroll          = NULL;
    pWndDesktop->pProp             = NULL;
    pWndDesktop->wIDmenu           = 0;
    pWndDesktop->helpContext       = 0;
    pWndDesktop->flags             = 0;
    pWndDesktop->hSysMenu          = 0;
    pWndDesktop->userdata          = 0;
    pWndDesktop->winproc           = winproc;
    pWndDesktop->cbWndExtra        = wndExtra;

    cs.lpCreateParams = NULL;
    cs.hInstance      = 0;
    cs.hMenu          = 0;
    cs.hwndParent     = 0;
    cs.x              = 0;
    cs.y              = 0;
    cs.cx             = pWndDesktop->rectWindow.right;
    cs.cy             = pWndDesktop->rectWindow.bottom;
    cs.style          = pWndDesktop->dwStyle;
    cs.dwExStyle      = pWndDesktop->dwExStyle;
    cs.lpszName       = NULL;
    cs.lpszClass      = DESKTOP_CLASS_ATOM;

    if (!USER_Driver.pCreateWindow( hwndDesktop, &cs, FALSE )) return FALSE;

    pWndDesktop->flags |= WIN_NEEDS_ERASEBKGND;
    WIN_ReleaseWndPtr( pWndDesktop );
    return TRUE;
}


/***********************************************************************
 *           WIN_FixCoordinates
 *
 * Fix the coordinates - Helper for WIN_CreateWindowEx.
 * returns default show mode in sw.
 * Note: the feature presented as undocumented *is* in the MSDN since 1993.
 */
static void WIN_FixCoordinates( CREATESTRUCTA *cs, INT *sw)
{
    if (cs->x == CW_USEDEFAULT || cs->x == CW_USEDEFAULT16 ||
        cs->cx == CW_USEDEFAULT || cs->cx == CW_USEDEFAULT16)
    {
        if (cs->style & (WS_CHILD | WS_POPUP))
        {
            if (cs->x == CW_USEDEFAULT || cs->x == CW_USEDEFAULT16) cs->x = cs->y = 0;
            if (cs->cx == CW_USEDEFAULT || cs->cx == CW_USEDEFAULT16) cs->cx = cs->cy = 0;
        }
        else  /* overlapped window */
        {
            STARTUPINFOA info;

            GetStartupInfoA( &info );

            if (cs->x == CW_USEDEFAULT || cs->x == CW_USEDEFAULT16)
            {
                /* Never believe Microsoft's documentation... CreateWindowEx doc says
                 * that if an overlapped window is created with WS_VISIBLE style bit
                 * set and the x parameter is set to CW_USEDEFAULT, the system ignores
                 * the y parameter. However, disassembling NT implementation (WIN32K.SYS)
                 * reveals that
                 *
                 * 1) not only it checks for CW_USEDEFAULT but also for CW_USEDEFAULT16
                 * 2) it does not ignore the y parameter as the docs claim; instead, it
                 *    uses it as second parameter to ShowWindow() unless y is either
                 *    CW_USEDEFAULT or CW_USEDEFAULT16.
                 *
                 * The fact that we didn't do 2) caused bogus windows pop up when wine
                 * was running apps that were using this obscure feature. Example -
                 * calc.exe that comes with Win98 (only Win98, it's different from
                 * the one that comes with Win95 and NT)
                 */
                if (cs->y != CW_USEDEFAULT && cs->y != CW_USEDEFAULT16) *sw = cs->y;
                cs->x = (info.dwFlags & STARTF_USEPOSITION) ? info.dwX : 0;
                cs->y = (info.dwFlags & STARTF_USEPOSITION) ? info.dwY : 0;
            }

            if (cs->cx == CW_USEDEFAULT || cs->cx == CW_USEDEFAULT16)
            {
                if (info.dwFlags & STARTF_USESIZE)
                {
                    cs->cx = info.dwXSize;
                    cs->cy = info.dwYSize;
                }
                else  /* if no other hint from the app, pick 3/4 of the screen real estate */
                {
                    RECT r;
                    SystemParametersInfoA( SPI_GETWORKAREA, 0, &r, 0);
                    cs->cx = (((r.right - r.left) * 3) / 4) - cs->x;
                    cs->cy = (((r.bottom - r.top) * 3) / 4) - cs->y;
                }
            }
        }
    }
}

/***********************************************************************
 *           WIN_CreateWindowEx
 *
 * Implementation of CreateWindowEx().
 */
static HWND WIN_CreateWindowEx( CREATESTRUCTA *cs, ATOM classAtom,
				WINDOWPROCTYPE type )
{
    INT sw = SW_SHOW;
    struct tagCLASS *classPtr;
    WND *wndPtr;
    HWND hwnd, hwndLinkAfter, parent, owner;
    POINT maxSize, maxPos, minTrack, maxTrack;
    INT wndExtra;
    DWORD clsStyle;
    WNDPROC winproc;
    DCE *dce;
    BOOL unicode = (type == WIN_PROC_32W);

    TRACE("%s %s ex=%08lx style=%08lx %d,%d %dx%d parent=%04x menu=%04x inst=%08x params=%p\n",
          (type == WIN_PROC_32W) ? debugres_w((LPWSTR)cs->lpszName) : debugres_a(cs->lpszName),
          (type == WIN_PROC_32W) ? debugres_w((LPWSTR)cs->lpszClass) : debugres_a(cs->lpszClass),
          cs->dwExStyle, cs->style, cs->x, cs->y, cs->cx, cs->cy,
          cs->hwndParent, cs->hMenu, cs->hInstance, cs->lpCreateParams );

    TRACE("winproc type is %d (%s)\n", type, (type == WIN_PROC_16) ? "WIN_PROC_16" :
	    ((type == WIN_PROC_32A) ? "WIN_PROC_32A" : "WIN_PROC_32W") );

    /* Find the parent window */

    parent = GetDesktopWindow();
    owner = 0;
    if (cs->hwndParent)
    {
	/* Make sure parent is valid */
        if (!IsWindow( cs->hwndParent ))
        {
            WARN("Bad parent %04x\n", cs->hwndParent );
	    return 0;
	}
        if (cs->style & WS_CHILD) parent = cs->hwndParent;
        else owner = GetAncestor( cs->hwndParent, GA_ROOT );
    }
    else if ((cs->style & WS_CHILD) && !(cs->style & WS_POPUP))
    {
        WARN("No parent for child window\n" );
        return 0;  /* WS_CHILD needs a parent, but WS_POPUP doesn't */
    }

    /* Find the window class */
    if (!(classPtr = CLASS_AddWindow( classAtom, cs->hInstance, type,
                                      &wndExtra, &winproc, &clsStyle, &dce )))
    {
        WARN("Bad class '%s'\n", cs->lpszClass );
        return 0;
    }

    WIN_FixCoordinates(cs, &sw); /* fix default coordinates */

    /* Correct the window style - stage 1
     *
     * These are patches that appear to affect both the style loaded into the
     * WIN structure and passed in the CreateStruct to the WM_CREATE etc.
     *
     * WS_EX_WINDOWEDGE appears to be enforced based on the other styles, so
     * why does the user get to set it?
     */

    /* This has been tested for WS_CHILD | WS_VISIBLE.  It has not been
     * tested for WS_POPUP
     */
    if ((cs->dwExStyle & WS_EX_DLGMODALFRAME) ||
        ((!(cs->dwExStyle & WS_EX_STATICEDGE)) &&
          (cs->style & (WS_DLGFRAME | WS_THICKFRAME))))
        cs->dwExStyle |= WS_EX_WINDOWEDGE;
    else
        cs->dwExStyle &= ~WS_EX_WINDOWEDGE;

    /* Create the window structure */

    if (!(wndPtr = create_window_handle( parent, owner,
                                         sizeof(*wndPtr) + wndExtra - sizeof(wndPtr->wExtra) )))
    {
	TRACE("out of memory\n" );
	return 0;
    }
    hwnd = wndPtr->hwndSelf;

    /* Fill the window structure */

    wndPtr->tid   = GetCurrentThreadId();
    wndPtr->next  = NULL;
    wndPtr->child = NULL;
    wndPtr->owner = owner;
    wndPtr->parent = WIN_FindWndPtr( parent );
    WIN_ReleaseWndPtr(wndPtr->parent);

    wndPtr->class          = classPtr;
    wndPtr->winproc        = winproc;
    wndPtr->hInstance      = cs->hInstance;
    wndPtr->text           = NULL;
    wndPtr->hmemTaskQ      = InitThreadInput16( 0, 0 );
    wndPtr->hrgnUpdate     = 0;
    wndPtr->hrgnWnd        = 0;
    wndPtr->hwndLastActive = hwnd;
    wndPtr->dwStyle        = cs->style & ~WS_VISIBLE;
    wndPtr->dwExStyle      = cs->dwExStyle;
    wndPtr->clsStyle       = clsStyle;
    wndPtr->wIDmenu        = 0;
    wndPtr->helpContext    = 0;
    wndPtr->flags          = (type == WIN_PROC_16) ? 0 : WIN_ISWIN32;
    wndPtr->pVScroll       = NULL;
    wndPtr->pHScroll       = NULL;
    wndPtr->pProp          = NULL;
    wndPtr->userdata       = 0;
    wndPtr->hSysMenu       = (wndPtr->dwStyle & WS_SYSMENU)
			     ? MENU_GetSysMenu( hwnd, 0 ) : 0;
    wndPtr->cbWndExtra     = wndExtra;

    if (wndExtra) memset( wndPtr->wExtra, 0, wndExtra);

    /* Call the WH_CBT hook */

    hwndLinkAfter = ((cs->style & (WS_CHILD|WS_MAXIMIZE)) == WS_CHILD)
 ? HWND_BOTTOM : HWND_TOP;

    if (HOOK_IsHooked( WH_CBT ))
    {
	CBT_CREATEWNDA cbtc;
        LRESULT ret;

	cbtc.lpcs = cs;
	cbtc.hwndInsertAfter = hwndLinkAfter;
        ret = (type == WIN_PROC_32W) ? HOOK_CallHooksW(WH_CBT, HCBT_CREATEWND,
                                                       (WPARAM)hwnd, (LPARAM)&cbtc)
                                     : HOOK_CallHooksA(WH_CBT, HCBT_CREATEWND,
                                                       (WPARAM)hwnd, (LPARAM)&cbtc);
        if (ret)
	{
	    TRACE("CBT-hook returned 0\n");
            free_window_handle( hwnd );
            CLASS_RemoveWindow( classPtr );
            hwnd =  0;
            goto end;
	}
    }

    /* Correct the window style - stage 2 */

    if (!(cs->style & WS_CHILD))
    {
	wndPtr->dwStyle |= WS_CLIPSIBLINGS;
	if (!(cs->style & WS_POPUP))
	{
            wndPtr->dwStyle |= WS_CAPTION;
            wndPtr->flags |= WIN_NEED_SIZE;
	}
    }

    /* Get class or window DC if needed */

    if (clsStyle & CS_OWNDC) wndPtr->dce = DCE_AllocDCE(hwnd,DCE_WINDOW_DC);
    else if (clsStyle & CS_CLASSDC) wndPtr->dce = dce;
    else wndPtr->dce = NULL;

    /* Initialize the dimensions before sending WM_GETMINMAXINFO */

    wndPtr->rectWindow.left   = cs->x;
    wndPtr->rectWindow.top    = cs->y;
    wndPtr->rectWindow.right  = cs->x + cs->cx;
    wndPtr->rectWindow.bottom = cs->y + cs->cy;
    wndPtr->rectClient        = wndPtr->rectWindow;

    /* Send the WM_GETMINMAXINFO message and fix the size if needed */

    if ((cs->style & WS_THICKFRAME) || !(cs->style & (WS_POPUP | WS_CHILD)))
    {
        WINPOS_GetMinMaxInfo( hwnd, &maxSize, &maxPos, &minTrack, &maxTrack);
        if (maxSize.x < cs->cx) cs->cx = maxSize.x;
        if (maxSize.y < cs->cy) cs->cy = maxSize.y;
        if (cs->cx < minTrack.x ) cs->cx = minTrack.x;
        if (cs->cy < minTrack.y ) cs->cy = minTrack.y;
    }

    if (cs->cx < 0) cs->cx = 0;
    if (cs->cy < 0) cs->cy = 0;

    wndPtr->rectWindow.left   = cs->x;
    wndPtr->rectWindow.top    = cs->y;
    wndPtr->rectWindow.right  = cs->x + cs->cx;
    wndPtr->rectWindow.bottom = cs->y + cs->cy;
    wndPtr->rectClient        = wndPtr->rectWindow;

    /* Set the window menu */

    if ((wndPtr->dwStyle & (WS_CAPTION | WS_CHILD)) == WS_CAPTION )
    {
        if (cs->hMenu) SetMenu(hwnd, cs->hMenu);
        else
        {
            LPCSTR menuName = (LPCSTR)GetClassLongA( hwnd, GCL_MENUNAME );
            if (menuName)
            {
                if (HIWORD(cs->hInstance))
                    cs->hMenu = LoadMenuA(cs->hInstance,menuName);
                else
                    cs->hMenu = LoadMenu16(cs->hInstance,menuName);

                if (cs->hMenu) SetMenu( hwnd, cs->hMenu );
            }
        }
    }
    else wndPtr->wIDmenu = (UINT)cs->hMenu;

    if (!USER_Driver.pCreateWindow( wndPtr->hwndSelf, cs, unicode))
    {
        WARN("aborted by WM_xxCREATE!\n");
        WIN_ReleaseWndPtr(WIN_DestroyWindow( wndPtr ));
        CLASS_RemoveWindow( classPtr );
        WIN_ReleaseWndPtr(wndPtr);
        return 0;
    }

    if( (wndPtr->dwStyle & WS_CHILD) && !(wndPtr->dwExStyle & WS_EX_NOPARENTNOTIFY) )
    {
        /* Notify the parent window only */

        SendMessageA( wndPtr->parent->hwndSelf, WM_PARENTNOTIFY,
                      MAKEWPARAM(WM_CREATE, wndPtr->wIDmenu), (LPARAM)hwnd );
        if( !IsWindow(hwnd) )
        {
            hwnd = 0;
            goto end;
        }
    }

    if (cs->style & WS_VISIBLE)
    {
        /* in case WS_VISIBLE got set in the meantime */
        wndPtr->dwStyle &= ~WS_VISIBLE;
        ShowWindow( hwnd, sw );
    }

    /* Call WH_SHELL hook */

    if (!(wndPtr->dwStyle & WS_CHILD) && !GetWindow( hwnd, GW_OWNER ))
        HOOK_CallHooksA( WH_SHELL, HSHELL_WINDOWCREATED, (WPARAM)hwnd, 0 );

    TRACE("created window %04x\n", hwnd);
 end:
    WIN_ReleaseWndPtr(wndPtr);
    return hwnd;
}


/***********************************************************************
 *		CreateWindow (USER.41)
 */
HWND16 WINAPI CreateWindow16( LPCSTR className, LPCSTR windowName,
                              DWORD style, INT16 x, INT16 y, INT16 width,
                              INT16 height, HWND16 parent, HMENU16 menu,
                              HINSTANCE16 instance, LPVOID data )
{
    return CreateWindowEx16( 0, className, windowName, style,
			   x, y, width, height, parent, menu, instance, data );
}


/***********************************************************************
 *		CreateWindowEx (USER.452)
 */
HWND16 WINAPI CreateWindowEx16( DWORD exStyle, LPCSTR className,
                                LPCSTR windowName, DWORD style, INT16 x,
                                INT16 y, INT16 width, INT16 height,
                                HWND16 parent, HMENU16 menu,
                                HINSTANCE16 instance, LPVOID data )
{
    ATOM classAtom;
    CREATESTRUCTA cs;
    char buffer[256];

    /* Find the class atom */

    if (HIWORD(className))
    {
        if (!(classAtom = GlobalFindAtomA( className )))
        {
            ERR( "bad class name %s\n", debugres_a(className) );
            return 0;
        }
    }
    else
    {
        classAtom = LOWORD(className);
        if (!GlobalGetAtomNameA( classAtom, buffer, sizeof(buffer) ))
        {
            ERR( "bad atom %x\n", classAtom);
            return 0;
        }
        className = buffer;
    }

    /* Fix the coordinates */

    cs.x  = (x == CW_USEDEFAULT16) ? CW_USEDEFAULT : (INT)x;
    cs.y  = (y == CW_USEDEFAULT16) ? CW_USEDEFAULT : (INT)y;
    cs.cx = (width == CW_USEDEFAULT16) ? CW_USEDEFAULT : (INT)width;
    cs.cy = (height == CW_USEDEFAULT16) ? CW_USEDEFAULT : (INT)height;

    /* Create the window */

    cs.lpCreateParams = data;
    cs.hInstance      = (HINSTANCE)instance;
    cs.hMenu          = (HMENU)menu;
    cs.hwndParent     = WIN_Handle32( parent );
    cs.style          = style;
    cs.lpszName       = windowName;
    cs.lpszClass      = className;
    cs.dwExStyle      = exStyle;

    return WIN_Handle16( WIN_CreateWindowEx( &cs, classAtom, WIN_PROC_16 ));
}


/***********************************************************************
 *		CreateWindowExA (USER32.@)
 */
HWND WINAPI CreateWindowExA( DWORD exStyle, LPCSTR className,
                                 LPCSTR windowName, DWORD style, INT x,
                                 INT y, INT width, INT height,
                                 HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID data )
{
    ATOM classAtom;
    CREATESTRUCTA cs;
    char buffer[256];

    if(!instance)
        instance=GetModuleHandleA(NULL);

    if(exStyle & WS_EX_MDICHILD)
        return CreateMDIWindowA(className, windowName, style, x, y, width, height, parent, instance, (LPARAM)data);

    /* Find the class atom */

    if (HIWORD(className))
    {
        if (!(classAtom = GlobalFindAtomA( className )))
        {
            ERR( "bad class name %s\n", debugres_a(className) );
            return 0;
        }
    }
    else
    {
        classAtom = LOWORD(className);
        if (!GlobalGetAtomNameA( classAtom, buffer, sizeof(buffer) ))
        {
            ERR( "bad atom %x\n", classAtom);
            return 0;
        }
        className = buffer;
    }

    /* Create the window */

    cs.lpCreateParams = data;
    cs.hInstance      = instance;
    cs.hMenu          = menu;
    cs.hwndParent     = parent;
    cs.x              = x;
    cs.y              = y;
    cs.cx             = width;
    cs.cy             = height;
    cs.style          = style;
    cs.lpszName       = windowName;
    cs.lpszClass      = className;
    cs.dwExStyle      = exStyle;

    return WIN_CreateWindowEx( &cs, classAtom, WIN_PROC_32A );
}


/***********************************************************************
 *		CreateWindowExW (USER32.@)
 */
HWND WINAPI CreateWindowExW( DWORD exStyle, LPCWSTR className,
                                 LPCWSTR windowName, DWORD style, INT x,
                                 INT y, INT width, INT height,
                                 HWND parent, HMENU menu,
                                 HINSTANCE instance, LPVOID data )
{
    ATOM classAtom;
    CREATESTRUCTW cs;
    WCHAR buffer[256];

    if(!instance)
        instance=GetModuleHandleA(NULL);

    if(exStyle & WS_EX_MDICHILD)
        return CreateMDIWindowW(className, windowName, style, x, y, width, height, parent, instance, (LPARAM)data);

    /* Find the class atom */

    if (HIWORD(className))
    {
        if (!(classAtom = GlobalFindAtomW( className )))
        {
            ERR( "bad class name %s\n", debugres_w(className) );
            return 0;
        }
    }
    else
    {
        classAtom = LOWORD(className);
        if (!GlobalGetAtomNameW( classAtom, buffer, sizeof(buffer)/sizeof(WCHAR) ))
        {
            ERR( "bad atom %x\n", classAtom);
            return 0;
        }
        className = buffer;
    }

    /* Create the window */

    cs.lpCreateParams = data;
    cs.hInstance      = instance;
    cs.hMenu          = menu;
    cs.hwndParent     = parent;
    cs.x              = x;
    cs.y              = y;
    cs.cx             = width;
    cs.cy             = height;
    cs.style          = style;
    cs.lpszName       = windowName;
    cs.lpszClass      = className;
    cs.dwExStyle      = exStyle;

    /* Note: we rely on the fact that CREATESTRUCTA and */
    /* CREATESTRUCTW have the same layout. */
    return WIN_CreateWindowEx( (CREATESTRUCTA *)&cs, classAtom, WIN_PROC_32W );
}


/***********************************************************************
 *           WIN_SendDestroyMsg
 */
static void WIN_SendDestroyMsg( HWND hwnd )
{
    if( CARET_GetHwnd() == hwnd) DestroyCaret();
    if (USER_Driver.pResetSelectionOwner)
        USER_Driver.pResetSelectionOwner( hwnd, TRUE );

    /*
     * Send the WM_DESTROY to the window.
     */
    SendMessageA( hwnd, WM_DESTROY, 0, 0);

    /*
     * This WM_DESTROY message can trigger re-entrant calls to DestroyWindow
     * make sure that the window still exists when we come back.
     */
    if (IsWindow(hwnd))
    {
        HWND* pWndArray;
        int i;

        if (!(pWndArray = WIN_ListChildren( hwnd ))) return;

        /* start from the end (FIXME: is this needed?) */
        for (i = 0; pWndArray[i]; i++) ;

        while (--i >= 0)
        {
            if (IsWindow( pWndArray[i] )) WIN_SendDestroyMsg( pWndArray[i] );
        }
        HeapFree( GetProcessHeap(), 0, pWndArray );
    }
    else
      WARN("\tdestroyed itself while in WM_DESTROY!\n");
}


/***********************************************************************
 *		DestroyWindow (USER32.@)
 */
BOOL WINAPI DestroyWindow( HWND hwnd )
{
    WND * wndPtr;
    BOOL retvalue;
    HWND h;

    hwnd = WIN_GetFullHandle( hwnd );
    TRACE("(%04x)\n", hwnd);

    /* Initialization */

    if (hwnd == GetDesktopWindow()) return FALSE;   /* Can't destroy desktop */

    /* Look whether the focus is within the tree of windows we will
     * be destroying.
     */
    h = GetFocus();
    if (h == hwnd || IsChild( hwnd, h ))
    {
        HWND parent = GetAncestor( hwnd, GA_PARENT );
        if (parent == GetDesktopWindow()) parent = 0;
        SetFocus( parent );
    }

      /* Call hooks */

    if( HOOK_CallHooksA( WH_CBT, HCBT_DESTROYWND, (WPARAM)hwnd, 0L) ) return FALSE;

    if (!(wndPtr = WIN_FindWndPtr( hwnd ))) return FALSE;
    if (!(wndPtr->dwStyle & WS_CHILD) && !GetWindow( hwnd, GW_OWNER ))
    {
        HOOK_CallHooksA( WH_SHELL, HSHELL_WINDOWDESTROYED, (WPARAM)hwnd, 0L );
        /* FIXME: clean up palette - see "Internals" p.352 */
    }

    if( !QUEUE_IsExitingQueue(wndPtr->hmemTaskQ) )
	if( wndPtr->dwStyle & WS_CHILD && !(wndPtr->dwExStyle & WS_EX_NOPARENTNOTIFY) )
	{
	    /* Notify the parent window only */
	    SendMessageA( wndPtr->parent->hwndSelf, WM_PARENTNOTIFY,
			    MAKEWPARAM(WM_DESTROY, wndPtr->wIDmenu), (LPARAM)hwnd );
            if( !IsWindow(hwnd) )
            {
                retvalue = TRUE;
                goto end;
            }
	}

    if (USER_Driver.pResetSelectionOwner)
        USER_Driver.pResetSelectionOwner( hwnd, FALSE ); /* before the window is unmapped */

      /* Hide the window */

    ShowWindow( hwnd, SW_HIDE );
    if (!IsWindow(hwnd))
    {
        retvalue = TRUE;
        goto end;
    }

      /* Recursively destroy owned windows */

    if( !(wndPtr->dwStyle & WS_CHILD) )
    {
        HWND owner;

      for (;;)
      {
          int i, got_one = 0;
          HWND *list = WIN_ListChildren( wndPtr->parent->hwndSelf );
          if (list)
          {
              for (i = 0; list[i]; i++)
              {
                  WND *siblingPtr;
                  if (GetWindow( list[i], GW_OWNER ) != hwnd) continue;
                  if (!(siblingPtr = WIN_FindWndPtr( list[i] ))) continue;
                  if (siblingPtr->hmemTaskQ == wndPtr->hmemTaskQ)
                  {
                      WIN_ReleaseWndPtr( siblingPtr );
                      DestroyWindow( list[i] );
                      got_one = 1;
                      continue;
                  }
                  else siblingPtr->owner = 0;
                  WIN_ReleaseWndPtr( siblingPtr );
              }
              HeapFree( GetProcessHeap(), 0, list );
          }
          if (!got_one) break;
      }

      WINPOS_ActivateOtherWindow( hwnd );

      if ((owner = GetWindow( hwnd, GW_OWNER )))
      {
          WND *ptr = WIN_FindWndPtr( owner );
          if (ptr)
          {
              if (ptr->hwndLastActive == hwnd) ptr->hwndLastActive = owner;
              WIN_ReleaseWndPtr( ptr );
          }
      }
    }

      /* Send destroy messages */

    WIN_SendDestroyMsg( hwnd );
    if (!IsWindow(hwnd))
    {
        retvalue = TRUE;
        goto end;
    }

      /* Unlink now so we won't bother with the children later on */

    if( wndPtr->parent ) WIN_UnlinkWindow(hwnd);

      /* Destroy the window storage */

    WIN_ReleaseWndPtr(WIN_DestroyWindow( wndPtr ));
    retvalue = TRUE;
end:
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}


/***********************************************************************
 *		CloseWindow (USER32.@)
 */
BOOL WINAPI CloseWindow( HWND hwnd )
{
    WND * wndPtr = WIN_FindWndPtr( hwnd );
    BOOL retvalue;

    if (!wndPtr || (wndPtr->dwStyle & WS_CHILD))
    {
        retvalue = FALSE;
        goto end;
    }
    ShowWindow( hwnd, SW_MINIMIZE );
    retvalue = TRUE;
end:
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;

}


/***********************************************************************
 *		OpenIcon (USER32.@)
 */
BOOL WINAPI OpenIcon( HWND hwnd )
{
    if (!IsIconic( hwnd )) return FALSE;
    ShowWindow( hwnd, SW_SHOWNORMAL );
    return TRUE;
}


/***********************************************************************
 *           WIN_FindWindow
 *
 * Implementation of FindWindow() and FindWindowEx().
 */
static HWND WIN_FindWindow( HWND parent, HWND child, ATOM className, LPCWSTR title )
{
    HWND *list;
    HWND retvalue;
    int i = 0, len = 0;
    WCHAR *buffer = NULL;

    if (!parent) parent = GetDesktopWindow();
    if (title)
    {
        len = strlenW(title) + 1;  /* one extra char to check for chars beyond the end */
        if (!(buffer = HeapAlloc( GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR) ))) return 0;
    }

    if (!(list = WIN_ListChildren( parent )))
    {
        if (buffer) HeapFree( GetProcessHeap(), 0, buffer );
        return 0;
    }

    if (child)
    {
        child = WIN_GetFullHandle( child );
        while (list[i] && list[i] != child) i++;
        if (!list[i]) return 0;
        i++;  /* start from next window */
    }

    for ( ; list[i]; i++)
    {
        if (className && (GetClassWord(list[i], GCW_ATOM) != className))
            continue;  /* Not the right class */

        /* Now check the title */
        if (!title) break;
        if (GetWindowTextW( list[i], buffer, len ) && !strcmpiW( buffer, title )) break;
    }
    retvalue = list[i];
    HeapFree( GetProcessHeap(), 0, list );
    if (buffer) HeapFree( GetProcessHeap(), 0, buffer );

    /* In this case we need to check whether other processes
       own a window with the given paramters on the Desktop,
       but we don't, so let's at least warn about it */
    if (!retvalue) FIXME("Returning 0 without checking other processes\n");
    return retvalue;
}



/***********************************************************************
 *		FindWindowA (USER32.@)
 */
HWND WINAPI FindWindowA( LPCSTR className, LPCSTR title )
{
    HWND ret = FindWindowExA( 0, 0, className, title );
    if (!ret) SetLastError (ERROR_CANNOT_FIND_WND_CLASS);
    return ret;
}


/***********************************************************************
 *		FindWindowExA (USER32.@)
 */
HWND WINAPI FindWindowExA( HWND parent, HWND child,
                               LPCSTR className, LPCSTR title )
{
    ATOM atom = 0;
    LPWSTR buffer;
    HWND hwnd;

    if (className)
    {
        /* If the atom doesn't exist, then no class */
        /* with this name exists either. */
        if (!(atom = GlobalFindAtomA( className )))
        {
            SetLastError (ERROR_CANNOT_FIND_WND_CLASS);
            return 0;
        }
    }

    buffer = HEAP_strdupAtoW( GetProcessHeap(), 0, title );
    hwnd = WIN_FindWindow( parent, child, atom, buffer );
    HeapFree( GetProcessHeap(), 0, buffer );
    return hwnd;
}


/***********************************************************************
 *		FindWindowExW (USER32.@)
 */
HWND WINAPI FindWindowExW( HWND parent, HWND child,
                               LPCWSTR className, LPCWSTR title )
{
    ATOM atom = 0;

    if (className)
    {
        /* If the atom doesn't exist, then no class */
        /* with this name exists either. */
        if (!(atom = GlobalFindAtomW( className )))
        {
            SetLastError (ERROR_CANNOT_FIND_WND_CLASS);
            return 0;
        }
    }
    return WIN_FindWindow( parent, child, atom, title );
}


/***********************************************************************
 *		FindWindowW (USER32.@)
 */
HWND WINAPI FindWindowW( LPCWSTR className, LPCWSTR title )
{
    return FindWindowExW( 0, 0, className, title );
}


/**********************************************************************
 *		GetDesktopWindow (USER32.@)
 */
HWND WINAPI GetDesktopWindow(void)
{
    if (pWndDesktop) return pWndDesktop->hwndSelf;
    ERR( "You need the -desktop option when running with native USER\n" );
    ExitProcess(1);
    return 0;
}


/*******************************************************************
 *		EnableWindow (USER32.@)
 */
BOOL WINAPI EnableWindow( HWND hwnd, BOOL enable )
{
    WND *wndPtr;
    BOOL retvalue;

    TRACE("( %x, %d )\n", hwnd, enable);

    if (USER_Driver.pEnableWindow)
        return USER_Driver.pEnableWindow( hwnd, enable );

    if (!(wndPtr = WIN_FindWndPtr( hwnd ))) return FALSE;
    hwnd = wndPtr->hwndSelf;  /* make it a full handle */

    retvalue = ((wndPtr->dwStyle & WS_DISABLED) != 0);

    if (enable && (wndPtr->dwStyle & WS_DISABLED))
    {
        wndPtr->dwStyle &= ~WS_DISABLED; /* Enable window */
        SendMessageA( hwnd, WM_ENABLE, TRUE, 0 );
    }
    else if (!enable && !(wndPtr->dwStyle & WS_DISABLED))
    {
        SendMessageA( hwnd, WM_CANCELMODE, 0, 0);

        wndPtr->dwStyle |= WS_DISABLED; /* Disable window */

        if (hwnd == GetFocus())
            SetFocus( 0 );  /* A disabled window can't have the focus */

        if (hwnd == GetCapture())
            ReleaseCapture();  /* A disabled window can't capture the mouse */

        SendMessageA( hwnd, WM_ENABLE, FALSE, 0 );
    }
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}


/***********************************************************************
 *		IsWindowEnabled (USER32.@)
 */
BOOL WINAPI IsWindowEnabled(HWND hWnd)
{
    WND * wndPtr;
    BOOL retvalue;

    if (!(wndPtr = WIN_FindWndPtr(hWnd))) return FALSE;
    retvalue = !(wndPtr->dwStyle & WS_DISABLED);
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;

}


/***********************************************************************
 *		IsWindowUnicode (USER32.@)
 */
BOOL WINAPI IsWindowUnicode( HWND hwnd )
{
    WND * wndPtr;
    BOOL retvalue;

    if (!(wndPtr = WIN_FindWndPtr(hwnd))) return FALSE;
    retvalue = (WINPROC_GetProcType( wndPtr->winproc ) == WIN_PROC_32W);
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}


/**********************************************************************
 *		GetWindowWord (USER32.@)
 */
WORD WINAPI GetWindowWord( HWND hwnd, INT offset )
{
    WORD retvalue;
    WND * wndPtr = WIN_FindWndPtr( hwnd );
    if (!wndPtr) return 0;
    if (offset >= 0)
    {
        if (offset + sizeof(WORD) > wndPtr->cbWndExtra)
        {
            WARN("Invalid offset %d\n", offset );
            retvalue = 0;
        }
        else retvalue = *(WORD *)(((char *)wndPtr->wExtra) + offset);
        WIN_ReleaseWndPtr(wndPtr);
        return retvalue;
    }

    WIN_ReleaseWndPtr(wndPtr);
    switch(offset)
    {
    case GWL_HWNDPARENT:
        return GetWindowLongW( hwnd, offset );
    case GWL_ID:
    case GWL_HINSTANCE:
        {
            LONG ret = GetWindowLongW( hwnd, offset );
            if (HIWORD(ret))
                WARN("%d: discards high bits of 0x%08lx!\n", offset, ret );
            return LOWORD(ret);
        }
    default:
        WARN("Invalid offset %d\n", offset );
        return 0;
    }
}


/**********************************************************************
 *		SetWindowWord (USER32.@)
 */
WORD WINAPI SetWindowWord( HWND hwnd, INT offset, WORD newval )
{
    WORD *ptr, retval;
    WND * wndPtr = WIN_FindWndPtr( hwnd );
    if (!wndPtr) return 0;
    if (offset >= 0)
    {
        if (offset + sizeof(WORD) > wndPtr->cbWndExtra)
        {
            WARN("Invalid offset %d\n", offset );
            WIN_ReleaseWndPtr(wndPtr);
            return 0;
        }
        ptr = (WORD *)(((char *)wndPtr->wExtra) + offset);
        retval = *ptr;
        *ptr = newval;
        WIN_ReleaseWndPtr(wndPtr);
        return retval;
    }

    WIN_ReleaseWndPtr(wndPtr);
    switch(offset)
    {
    case GWL_ID:
    case GWL_HINSTANCE:
    case GWL_HWNDPARENT:
        return SetWindowLongW( hwnd, offset, (UINT)newval );
    default:
        WARN("Invalid offset %d\n", offset );
        return 0;
    }
}


/**********************************************************************
 *	     WIN_GetWindowLong
 *
 * Helper function for GetWindowLong().
 */
static LONG WIN_GetWindowLong( HWND hwnd, INT offset, WINDOWPROCTYPE type )
{
    LONG retvalue;
    WND * wndPtr = WIN_FindWndPtr( hwnd );
    if (!wndPtr) return 0;
    if (offset >= 0)
    {
        if (offset + sizeof(LONG) > wndPtr->cbWndExtra)
        {
            WARN("Invalid offset %d\n", offset );
            retvalue = 0;
            goto end;
        }
        retvalue = *(LONG *)(((char *)wndPtr->wExtra) + offset);
        /* Special case for dialog window procedure */
        if ((offset == DWL_DLGPROC) && (wndPtr->flags & WIN_ISDIALOG))
        {
            retvalue = (LONG)WINPROC_GetProc( (HWINDOWPROC)retvalue, type );
            goto end;
    }
        goto end;
    }
    switch(offset)
    {
        case GWL_USERDATA:   retvalue = wndPtr->userdata;
                             goto end;
        case GWL_STYLE:      retvalue = wndPtr->dwStyle;
                             goto end;
        case GWL_EXSTYLE:    retvalue = wndPtr->dwExStyle;
                             goto end;
        case GWL_ID:         retvalue = (LONG)wndPtr->wIDmenu;
                             goto end;
        case GWL_WNDPROC:    retvalue = (LONG)WINPROC_GetProc( wndPtr->winproc,
                                                           type );
                             goto end;
        case GWL_HWNDPARENT: retvalue = (LONG)GetParent(hwnd);
                             goto end;
        case GWL_HINSTANCE:  retvalue = wndPtr->hInstance;
                             goto end;
        default:
            WARN("Unknown offset %d\n", offset );
    }
    retvalue = 0;
end:
    WIN_ReleaseWndPtr(wndPtr);
    return retvalue;
}


/**********************************************************************
 *	     WIN_SetWindowLong
 *
 * Helper function for SetWindowLong().
 *
 * 0 is the failure code. However, in the case of failure SetLastError
 * must be set to distinguish between a 0 return value and a failure.
 *
 * FIXME: The error values for SetLastError may not be right. Can
 *        someone check with the real thing?
 */
static LONG WIN_SetWindowLong( HWND hwnd, INT offset, LONG newval,
                               WINDOWPROCTYPE type )
{
    LONG *ptr, retval;
    WND * wndPtr = WIN_FindWndPtr( hwnd );
    STYLESTRUCT style;

    TRACE("%x=%p %x %lx %x\n",hwnd, wndPtr, offset, newval, type);

    if (!wndPtr)
    {
       /* Is this the right error? */
       SetLastError( ERROR_INVALID_WINDOW_HANDLE );
       return 0;
    }

    if (offset >= 0)
    {
        if (offset + sizeof(LONG) > wndPtr->cbWndExtra)
        {
            WARN("Invalid offset %d\n", offset );

            /* Is this the right error? */
            SetLastError( ERROR_OUTOFMEMORY );

            retval = 0;
            goto end;
        }
        ptr = (LONG *)(((char *)wndPtr->wExtra) + offset);
        /* Special case for dialog window procedure */
        if ((offset == DWL_DLGPROC) && (wndPtr->flags & WIN_ISDIALOG))
        {
            retval = (LONG)WINPROC_GetProc( (HWINDOWPROC)*ptr, type );
            WINPROC_SetProc( (HWINDOWPROC *)ptr, (WNDPROC16)newval,
                             type, WIN_PROC_WINDOW );
            goto end;
        }
    }
    else switch(offset)
    {
	case GWL_ID:
		ptr = (DWORD*)&wndPtr->wIDmenu;
		break;
	case GWL_HINSTANCE:
                ptr = (DWORD*)&wndPtr->hInstance;
                break;
        case GWL_USERDATA:
                ptr = &wndPtr->userdata;
                break;
        case GWL_HWNDPARENT:
                retval = SetParent( hwnd, (HWND)newval );
                goto end;
	case GWL_WNDPROC:
		retval = (LONG)WINPROC_GetProc( wndPtr->winproc, type );
		WINPROC_SetProc( &wndPtr->winproc, (WNDPROC16)newval,
						type, WIN_PROC_WINDOW );
		goto end;
	case GWL_STYLE:
	       	style.styleOld = wndPtr->dwStyle;
		style.styleNew = newval;
                SendMessageA(hwnd,WM_STYLECHANGING,GWL_STYLE,(LPARAM)&style);
		wndPtr->dwStyle = style.styleNew;
                SendMessageA(hwnd,WM_STYLECHANGED,GWL_STYLE,(LPARAM)&style);
                retval = style.styleOld;
                goto end;
        case GWL_EXSTYLE:
	        style.styleOld = wndPtr->dwExStyle;
		style.styleNew = newval;
                SendMessageA(hwnd,WM_STYLECHANGING,GWL_EXSTYLE,(LPARAM)&style);
		wndPtr->dwExStyle = style.styleNew;
                SendMessageA(hwnd,WM_STYLECHANGED,GWL_EXSTYLE,(LPARAM)&style);
                retval = style.styleOld;
                goto end;

	default:
            WARN("Invalid offset %d\n", offset );

            /* Don't think this is right error but it should do */
            SetLastError( ERROR_OUTOFMEMORY );

            retval = 0;
            goto end;
    }
    retval = *ptr;
    *ptr = newval;
end:
    WIN_ReleaseWndPtr(wndPtr);
    return retval;
}


/**********************************************************************
 *		GetWindowLong (USER.135)
 */
LONG WINAPI GetWindowLong16( HWND16 hwnd, INT16 offset )
{
    return WIN_GetWindowLong( WIN_Handle32(hwnd), offset, WIN_PROC_16 );
}


/**********************************************************************
 *		GetWindowLongA (USER32.@)
 */
LONG WINAPI GetWindowLongA( HWND hwnd, INT offset )
{
    return WIN_GetWindowLong( hwnd, offset, WIN_PROC_32A );
}


/**********************************************************************
 *		GetWindowLongW (USER32.@)
 */
LONG WINAPI GetWindowLongW( HWND hwnd, INT offset )
{
    return WIN_GetWindowLong( hwnd, offset, WIN_PROC_32W );
}


/**********************************************************************
 *		SetWindowLong (USER.136)
 */
LONG WINAPI SetWindowLong16( HWND16 hwnd, INT16 offset, LONG newval )
{
    return WIN_SetWindowLong( WIN_Handle32(hwnd), offset, newval, WIN_PROC_16 );
}


/**********************************************************************
 *		SetWindowLongA (USER32.@)
 */
LONG WINAPI SetWindowLongA( HWND hwnd, INT offset, LONG newval )
{
    return WIN_SetWindowLong( hwnd, offset, newval, WIN_PROC_32A );
}


/**********************************************************************
 *		SetWindowLongW (USER32.@) Set window attribute
 *
 * SetWindowLong() alters one of a window's attributes or sets a 32-bit (long)
 * value in a window's extra memory.
 *
 * The _hwnd_ parameter specifies the window.  is the handle to a
 * window that has extra memory. The _newval_ parameter contains the
 * new attribute or extra memory value.  If positive, the _offset_
 * parameter is the byte-addressed location in the window's extra
 * memory to set.  If negative, _offset_ specifies the window
 * attribute to set, and should be one of the following values:
 *
 * GWL_EXSTYLE      The window's extended window style
 *
 * GWL_STYLE        The window's window style.
 *
 * GWL_WNDPROC      Pointer to the window's window procedure.
 *
 * GWL_HINSTANCE    The window's pplication instance handle.
 *
 * GWL_ID           The window's identifier.
 *
 * GWL_USERDATA     The window's user-specified data.
 *
 * If the window is a dialog box, the _offset_ parameter can be one of
 * the following values:
 *
 * DWL_DLGPROC      The address of the window's dialog box procedure.
 *
 * DWL_MSGRESULT    The return value of a message
 *                  that the dialog box procedure processed.
 *
 * DWL_USER         Application specific information.
 *
 * RETURNS
 *
 * If successful, returns the previous value located at _offset_. Otherwise,
 * returns 0.
 *
 * NOTES
 *
 * Extra memory for a window class is specified by a nonzero cbWndExtra
 * parameter of the WNDCLASS structure passed to RegisterClass() at the
 * time of class creation.
 *
 * Using GWL_WNDPROC to set a new window procedure effectively creates
 * a window subclass. Use CallWindowProc() in the new windows procedure
 * to pass messages to the superclass's window procedure.
 *
 * The user data is reserved for use by the application which created
 * the window.
 *
 * Do not use GWL_STYLE to change the window's WS_DISABLE style;
 * instead, call the EnableWindow() function to change the window's
 * disabled state.
 *
 * Do not use GWL_HWNDPARENT to reset the window's parent, use
 * SetParent() instead.
 *
 * Win95:
 * When offset is GWL_STYLE and the calling app's ver is 4.0,
 * it sends WM_STYLECHANGING before changing the settings
 * and WM_STYLECHANGED afterwards.
 * App ver 4.0 can't use SetWindowLong to change WS_EX_TOPMOST.
 *
 * BUGS
 *
 * GWL_STYLE does not dispatch WM_STYLE... messages.
 *
 * CONFORMANCE
 *
 * ECMA-234, Win32
 *
 */
LONG WINAPI SetWindowLongW(
    HWND hwnd,  /* [in] window to alter */
    INT offset, /* [in] offset, in bytes, of location to alter */
    LONG newval /* [in] new value of location */
) {
    return WIN_SetWindowLong( hwnd, offset, newval, WIN_PROC_32W );
}


/*******************************************************************
 *		GetWindowTextA (USER32.@)
 */
INT WINAPI GetWindowTextA( HWND hwnd, LPSTR lpString, INT nMaxCount )
{
    return (INT)SendMessageA( hwnd, WM_GETTEXT, nMaxCount,
                                  (LPARAM)lpString );
}

/*******************************************************************
 *		InternalGetWindowText (USER32.@)
 */
INT WINAPI InternalGetWindowText(HWND hwnd,LPWSTR lpString,INT nMaxCount )
{
    WND *win = WIN_FindWndPtr( hwnd );
    if (!win) return 0;
    if (win->text) lstrcpynW( lpString, win->text, nMaxCount );
    else lpString[0] = 0;
    WIN_ReleaseWndPtr( win );
    return strlenW(lpString);
}


/*******************************************************************
 *		GetWindowTextW (USER32.@)
 */
INT WINAPI GetWindowTextW( HWND hwnd, LPWSTR lpString, INT nMaxCount )
{
    return (INT)SendMessageW( hwnd, WM_GETTEXT, nMaxCount,
                                  (LPARAM)lpString );
}


/*******************************************************************
 *		SetWindowText  (USER32.@)
 *		SetWindowTextA (USER32.@)
 */
BOOL WINAPI SetWindowTextA( HWND hwnd, LPCSTR lpString )
{
    return (BOOL)SendMessageA( hwnd, WM_SETTEXT, 0, (LPARAM)lpString );
}


/*******************************************************************
 *		SetWindowTextW (USER32.@)
 */
BOOL WINAPI SetWindowTextW( HWND hwnd, LPCWSTR lpString )
{
    return (BOOL)SendMessageW( hwnd, WM_SETTEXT, 0, (LPARAM)lpString );
}


/*******************************************************************
 *		GetWindowTextLengthA (USER32.@)
 */
INT WINAPI GetWindowTextLengthA( HWND hwnd )
{
    return SendMessageA( hwnd, WM_GETTEXTLENGTH, 0, 0 );
}

/*******************************************************************
 *		GetWindowTextLengthW (USER32.@)
 */
INT WINAPI GetWindowTextLengthW( HWND hwnd )
{
    return SendMessageW( hwnd, WM_GETTEXTLENGTH, 0, 0 );
}


/*******************************************************************
 *		IsWindow (USER32.@)
 */
BOOL WINAPI IsWindow( HWND hwnd )
{
    WND *ptr;
    BOOL ret;

    USER_Lock();
    if ((ptr = user_handles[LOWORD(hwnd)]))
    {
        ret = ((ptr->dwMagic == WND_MAGIC) && (!HIWORD(hwnd) || hwnd == ptr->hwndSelf));
        USER_Unlock();
        return ret;
    }
    USER_Unlock();

    /* check other processes */
    SERVER_START_REQ( get_window_info )
    {
        req->handle = hwnd;
        ret = !SERVER_CALL_ERR();
    }
    SERVER_END_REQ;
    return ret;
}


/***********************************************************************
 *		GetWindowThreadProcessId (USER32.@)
 */
DWORD WINAPI GetWindowThreadProcessId( HWND hwnd, LPDWORD process )
{
    WND *ptr;
    DWORD tid = 0;

    USER_Lock();
    if ((ptr = user_handles[LOWORD(hwnd)]))
    {
        if ((ptr->dwMagic == WND_MAGIC) && (!HIWORD(hwnd) || hwnd == ptr->hwndSelf))
        {
            /* got a valid window */
            tid = ptr->tid;
            if (process) *process = GetCurrentProcessId();
        }
        else SetLastError( ERROR_INVALID_WINDOW_HANDLE);
        USER_Unlock();
        return tid;
    }
    USER_Unlock();

    /* check other processes */
    SERVER_START_REQ( get_window_info )
    {
        req->handle = hwnd;
        if (!SERVER_CALL_ERR())
        {
            tid = (DWORD)req->tid;
            if (process) *process = (DWORD)req->pid;
        }
    }
    SERVER_END_REQ;
    return tid;
}


/*****************************************************************
 *		GetParent (USER32.@)
 */
HWND WINAPI GetParent( HWND hwnd )
{
    WND *wndPtr;
    HWND retvalue = 0;

    if ((wndPtr = WIN_FindWndPtr(hwnd)))
    {
        if (wndPtr->dwStyle & WS_CHILD)
            retvalue = wndPtr->parent->hwndSelf;
        else if (wndPtr->dwStyle & WS_POPUP)
            retvalue = wndPtr->owner;
        WIN_ReleaseWndPtr(wndPtr);
    }
    return retvalue;
}


/*****************************************************************
 *		GetAncestor (USER32.@)
 */
HWND WINAPI GetAncestor( HWND hwnd, UINT type )
{
    HWND ret = 0;
    WND *wndPtr;

    if (!(wndPtr = WIN_FindWndPtr(hwnd))) return 0;
    if (wndPtr->hwndSelf == GetDesktopWindow()) goto done;

    switch(type)
    {
    case GA_PARENT:
        WIN_UpdateWndPtr( &wndPtr, wndPtr->parent );
        break;
    case GA_ROOT:
        while (wndPtr->parent->hwndSelf != GetDesktopWindow())
            WIN_UpdateWndPtr( &wndPtr, wndPtr->parent );
        break;
    case GA_ROOTOWNER:
        while (wndPtr->parent->hwndSelf != GetDesktopWindow())
            WIN_UpdateWndPtr( &wndPtr, wndPtr->parent );
        while (wndPtr && wndPtr->owner)
        {
            WND *ptr = WIN_FindWndPtr( wndPtr->owner );
            WIN_ReleaseWndPtr( wndPtr );
            wndPtr = ptr;
        }
        break;
    }
    ret = wndPtr ? wndPtr->hwndSelf : 0;
 done:
    WIN_ReleaseWndPtr( wndPtr );
    return ret;
}


/*****************************************************************
 *		SetParent (USER32.@)
 */
HWND WINAPI SetParent( HWND hwnd, HWND parent )
{
    WND *wndPtr;
    DWORD dwStyle;
    HWND retvalue;

    if (!parent) parent = GetDesktopWindow();
    else parent = WIN_GetFullHandle( parent );

    /* sanity checks */
    if (WIN_GetFullHandle(hwnd) == GetDesktopWindow() || !IsWindow( parent ))
    {
        SetLastError( ERROR_INVALID_WINDOW_HANDLE );
        return 0;
    }

    if (USER_Driver.pSetParent)
        return USER_Driver.pSetParent( hwnd, parent );

    if (!(wndPtr = WIN_FindWndPtr(hwnd))) return 0;

    dwStyle = wndPtr->dwStyle;

    /* Windows hides the window first, then shows it again
     * including the WM_SHOWWINDOW messages and all */
    if (dwStyle & WS_VISIBLE) ShowWindow( hwnd, SW_HIDE );

    retvalue = wndPtr->parent->hwndSelf;  /* old parent */
    if (parent != retvalue)
    {
        WIN_LinkWindow( hwnd, parent, HWND_TOP );

        if (parent != GetDesktopWindow()) /* a child window */
        {
            if (!(dwStyle & WS_CHILD))
            {
                HMENU menu = (HMENU)SetWindowLongW( hwnd, GWL_ID, 0 );
                if (menu) DestroyMenu( menu );
            }
        }
    }
    WIN_ReleaseWndPtr( wndPtr );

    /* SetParent additionally needs to make hwnd the topmost window
       in the x-order and send the expected WM_WINDOWPOSCHANGING and
       WM_WINDOWPOSCHANGED notification messages.
    */
    SetWindowPos( hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOACTIVATE|SWP_NOMOVE|SWP_NOSIZE|
                  ((dwStyle & WS_VISIBLE)?SWP_SHOWWINDOW:0));
    /* FIXME: a WM_MOVE is also generated (in the DefWindowProc handler
     * for WM_WINDOWPOSCHANGED) in Windows, should probably remove SWP_NOMOVE */
    return retvalue;
}


/*******************************************************************
 *		IsChild (USER32.@)
 */
BOOL WINAPI IsChild( HWND parent, HWND child )
{
    HWND *list = WIN_ListParents( child );
    int i;
    BOOL ret;

    if (!list) return FALSE;
    parent = WIN_GetFullHandle( parent );
    for (i = 0; list[i]; i++) if (list[i] == parent) break;
    ret = (list[i] != 0);
    HeapFree( GetProcessHeap(), 0, list );
    return ret;
}


/***********************************************************************
 *		IsWindowVisible (USER32.@)
 */
BOOL WINAPI IsWindowVisible( HWND hwnd )
{
    HWND *list;
    BOOL retval;
    int i;

    if (!(GetWindowLongW( hwnd, GWL_STYLE ) & WS_VISIBLE)) return FALSE;
    if (!(list = WIN_ListParents( hwnd ))) return TRUE;
    for (i = 0; list[i]; i++)
        if (!(GetWindowLongW( list[i], GWL_STYLE ) & WS_VISIBLE)) break;
    retval = !list[i];
    HeapFree( GetProcessHeap(), 0, list );
    return retval;
}


/***********************************************************************
 *           WIN_IsWindowDrawable
 *
 * hwnd is drawable when it is visible, all parents are not
 * minimized, and it is itself not minimized unless we are
 * trying to draw its default class icon.
 */
BOOL WIN_IsWindowDrawable( WND* wnd, BOOL icon )
{
    HWND *list;
    BOOL retval;
    int i;

    if (!(wnd->dwStyle & WS_VISIBLE)) return FALSE;
    if ((wnd->dwStyle & WS_MINIMIZE) &&
        icon && GetClassLongA( wnd->hwndSelf, GCL_HICON ))  return FALSE;

    if (!(list = WIN_ListParents( wnd->hwndSelf ))) return TRUE;
    for (i = 0; list[i]; i++)
        if ((GetWindowLongW( list[i], GWL_STYLE ) & (WS_VISIBLE|WS_MINIMIZE)) != WS_VISIBLE)
            break;
    retval = !list[i];
    HeapFree( GetProcessHeap(), 0, list );
    return retval;
}


/*******************************************************************
 *		GetTopWindow (USER32.@)
 */
HWND WINAPI GetTopWindow( HWND hwnd )
{
    if (!hwnd) hwnd = GetDesktopWindow();
    return GetWindow( hwnd, GW_CHILD );
}


/*******************************************************************
 *		GetWindow (USER32.@)
 */
HWND WINAPI GetWindow( HWND hwnd, UINT rel )
{
    HWND retval = 0;

    if (rel == GW_OWNER)  /* special case: not fully supported in the server yet */
    {
        WND *wndPtr = WIN_FindWndPtr( hwnd );
        if (!wndPtr) return 0;
        retval = wndPtr->owner;
        WIN_ReleaseWndPtr( wndPtr );
        return retval;
    }

    SERVER_START_REQ( get_window_tree )
    {
        req->handle = hwnd;
        if (!SERVER_CALL_ERR())
        {
            switch(rel)
            {
            case GW_HWNDFIRST:
                retval = req->first_sibling;
                break;
            case GW_HWNDLAST:
                retval = req->last_sibling;
                break;
            case GW_HWNDNEXT:
                retval = req->next_sibling;
                break;
            case GW_HWNDPREV:
                retval = req->prev_sibling;
                break;
            case GW_CHILD:
                retval = req->first_child;
                break;
            }
        }
    }
    SERVER_END_REQ;
    return retval;
}


/***********************************************************************
 *           WIN_InternalShowOwnedPopups
 *
 * Internal version of ShowOwnedPopups; Wine functions should use this
 * to avoid interfering with application calls to ShowOwnedPopups
 * and to make sure the application can't prevent showing/hiding.
 *
 * Set unmanagedOnly to TRUE to show/hide unmanaged windows only.
 *
 */

BOOL WIN_InternalShowOwnedPopups( HWND owner, BOOL fShow, BOOL unmanagedOnly )
{
    int count = 0;
    WND *pWnd;
    HWND *win_array = WIN_ListChildren( GetDesktopWindow() );

    if (!win_array) return TRUE;

    /*
     * Show windows Lowest first, Highest last to preserve Z-Order
     */
    while (win_array[count]) count++;
    while (--count >= 0)
    {
        if (GetWindow( win_array[count], GW_OWNER ) != owner) continue;
        if (!(pWnd = WIN_FindWndPtr( win_array[count] ))) continue;

        if (pWnd->dwStyle & WS_POPUP)
        {
            if (fShow)
            {
                /* check in window was flagged for showing in previous WIN_InternalShowOwnedPopups call */
                if (pWnd->flags & WIN_NEEDS_INTERNALSOP)
                {
                    /*
                     * Call ShowWindow directly because an application can intercept WM_SHOWWINDOW messages
                     */
                    ShowWindow(pWnd->hwndSelf,SW_SHOW);
                    pWnd->flags &= ~WIN_NEEDS_INTERNALSOP; /* remove the flag */
                }
            }
            else
            {
                if ( IsWindowVisible(pWnd->hwndSelf) &&                   /* hide only if window is visible */
                     !( pWnd->flags & WIN_NEEDS_INTERNALSOP ) &&          /* don't hide if previous call already did it */
                     !( unmanagedOnly && (pWnd->dwExStyle & WS_EX_MANAGED) ) ) /* don't hide managed windows if unmanagedOnly is TRUE */
                {
                    /*
                     * Call ShowWindow directly because an application can intercept WM_SHOWWINDOW messages
                     */
                    ShowWindow(pWnd->hwndSelf,SW_HIDE);
                    /* flag the window for showing on next WIN_InternalShowOwnedPopups call */
                    pWnd->flags |= WIN_NEEDS_INTERNALSOP;
                }
            }
        }
        WIN_ReleaseWndPtr( pWnd );
    }
    HeapFree( GetProcessHeap(), 0, win_array );

    return TRUE;
}

/*******************************************************************
 *		ShowOwnedPopups (USER32.@)
 */
BOOL WINAPI ShowOwnedPopups( HWND owner, BOOL fShow )
{
    int count = 0;
    WND *pWnd;
    HWND *win_array = WIN_ListChildren( GetDesktopWindow() );

    if (!win_array) return TRUE;

    while (win_array[count]) count++;
    while (--count >= 0)
    {
        if (GetWindow( win_array[count], GW_OWNER ) != owner) continue;
        if (!(pWnd = WIN_FindWndPtr( win_array[count] ))) continue;

        if (pWnd->dwStyle & WS_POPUP)
        {
            if (fShow)
            {
                if (pWnd->flags & WIN_NEEDS_SHOW_OWNEDPOPUP)
                {
                    /* In Windows, ShowOwnedPopups(TRUE) generates
                     * WM_SHOWWINDOW messages with SW_PARENTOPENING,
                     * regardless of the state of the owner
                     */
                    SendMessageA(pWnd->hwndSelf, WM_SHOWWINDOW, SW_SHOW, SW_PARENTOPENING);
                    pWnd->flags &= ~WIN_NEEDS_SHOW_OWNEDPOPUP;
                }
            }
            else
            {
                if (IsWindowVisible(pWnd->hwndSelf))
                {
                    /* In Windows, ShowOwnedPopups(FALSE) generates
                     * WM_SHOWWINDOW messages with SW_PARENTCLOSING,
                     * regardless of the state of the owner
                     */
                    SendMessageA(pWnd->hwndSelf, WM_SHOWWINDOW, SW_HIDE, SW_PARENTCLOSING);
                    pWnd->flags |= WIN_NEEDS_SHOW_OWNEDPOPUP;
                }
            }
        }
        WIN_ReleaseWndPtr( pWnd );
    }
    HeapFree( GetProcessHeap(), 0, win_array );
    return TRUE;
}


/*******************************************************************
 *		GetLastActivePopup (USER32.@)
 */
HWND WINAPI GetLastActivePopup( HWND hwnd )
{
    HWND retval;
    WND *wndPtr =WIN_FindWndPtr(hwnd);
    if (!wndPtr) return hwnd;
    retval = wndPtr->hwndLastActive;
    if (!IsWindow( retval )) retval = wndPtr->hwndSelf;
    WIN_ReleaseWndPtr(wndPtr);
    return retval;
}


/*******************************************************************
 *           WIN_ListParents
 *
 * Build an array of all parents of a given window, starting with
 * the immediate parent. The array must be freed with HeapFree.
 * Returns NULL if window is a top-level window.
 */
HWND *WIN_ListParents( HWND hwnd )
{
    HWND *list = NULL;

    SERVER_START_VAR_REQ( get_window_parents, REQUEST_MAX_VAR_SIZE )
    {
        req->handle = hwnd;
        if (!SERVER_CALL())
        {
            user_handle_t *data = server_data_ptr(req);
            int i, count = server_data_size(req) / sizeof(*data);
            if (count && ((list = HeapAlloc( GetProcessHeap(), 0, (count + 1) * sizeof(HWND) ))))
            {
                for (i = 0; i < count; i++) list[i] = data[i];
                list[i] = 0;
            }
        }
    }
    SERVER_END_VAR_REQ;
    return list;
}


/*******************************************************************
 *           WIN_ListChildren
 *
 * Build an array of the children of a given window. The array must be
 * freed with HeapFree. Returns NULL when no windows are found.
 */
HWND *WIN_ListChildren( HWND hwnd )
{
    HWND *list = NULL;

    SERVER_START_VAR_REQ( get_window_children, REQUEST_MAX_VAR_SIZE )
    {
        req->parent = hwnd;
        if (!SERVER_CALL())
        {
            user_handle_t *data = server_data_ptr(req);
            int i, count = server_data_size(req) / sizeof(*data);
            if (count && ((list = HeapAlloc( GetProcessHeap(), 0, (count + 1) * sizeof(HWND) ))))
            {
                for (i = 0; i < count; i++) list[i] = data[i];
                list[i] = 0;
            }
        }
    }
    SERVER_END_VAR_REQ;
    return list;
}


/*******************************************************************
 *		EnumWindows (USER32.@)
 */
BOOL WINAPI EnumWindows( WNDENUMPROC lpEnumFunc, LPARAM lParam )
{
    HWND *list;
    BOOL ret = TRUE;
    int i, iWndsLocks;

    /* We have to build a list of all windows first, to avoid */
    /* unpleasant side-effects, for instance if the callback */
    /* function changes the Z-order of the windows.          */

    if (!(list = WIN_ListChildren( GetDesktopWindow() ))) return FALSE;

    /* Now call the callback function for every window */

    iWndsLocks = WIN_SuspendWndsLock();
    for (i = 0; list[i]; i++)
    {
        /* Make sure that the window still exists */
        if (!IsWindow( list[i] )) continue;
        if (!(ret = lpEnumFunc( list[i], lParam ))) break;
    }
    WIN_RestoreWndsLock(iWndsLocks);
    HeapFree( GetProcessHeap(), 0, list );
    return ret;
}


/**********************************************************************
 *		EnumTaskWindows16   (USER.225)
 */
BOOL16 WINAPI EnumTaskWindows16( HTASK16 hTask, WNDENUMPROC16 func,
                                 LPARAM lParam )
{
    TDB *tdb = TASK_GetPtr( hTask );
    if (!tdb) return FALSE;
    return EnumThreadWindows( (DWORD)tdb->teb->tid, (WNDENUMPROC)func, lParam );
}


/**********************************************************************
 *		EnumThreadWindows (USER32.@)
 */
BOOL WINAPI EnumThreadWindows( DWORD id, WNDENUMPROC func, LPARAM lParam )
{
    HWND *list;
    int i, iWndsLocks;

    if (!(list = WIN_ListChildren( GetDesktopWindow() ))) return FALSE;

    /* Now call the callback function for every window */

    iWndsLocks = WIN_SuspendWndsLock();
    for (i = 0; list[i]; i++)
    {
        if (GetWindowThreadProcessId( list[i], NULL ) != id) continue;
        if (!func( list[i], lParam )) break;
    }
    WIN_RestoreWndsLock(iWndsLocks);
    HeapFree( GetProcessHeap(), 0, list );
    return TRUE;
}


/**********************************************************************
 *           WIN_EnumChildWindows
 *
 * Helper function for EnumChildWindows().
 */
static BOOL WIN_EnumChildWindows( HWND *list, WNDENUMPROC func, LPARAM lParam )
{
    HWND *childList;
    BOOL ret = FALSE;

    for ( ; *list; list++)
    {
        /* Make sure that the window still exists */
        if (!IsWindow( *list )) continue;
        /* skip owned windows */
        if (GetWindow( *list, GW_OWNER )) continue;
        /* Build children list first */
        childList = WIN_ListChildren( *list );

        ret = func( *list, lParam );

        if (childList)
        {
            if (ret) ret = WIN_EnumChildWindows( childList, func, lParam );
            HeapFree( GetProcessHeap(), 0, childList );
        }
        if (!ret) return FALSE;
    }
    return TRUE;
}


/**********************************************************************
 *		EnumChildWindows (USER32.@)
 */
BOOL WINAPI EnumChildWindows( HWND parent, WNDENUMPROC func, LPARAM lParam )
{
    HWND *list;
    int iWndsLocks;

    if (!(list = WIN_ListChildren( parent ))) return FALSE;
    iWndsLocks = WIN_SuspendWndsLock();
    WIN_EnumChildWindows( list, func, lParam );
    WIN_RestoreWndsLock(iWndsLocks);
    HeapFree( GetProcessHeap(), 0, list );
    return TRUE;
}


/*******************************************************************
 *		AnyPopup (USER.52)
 */
BOOL16 WINAPI AnyPopup16(void)
{
    return AnyPopup();
}


/*******************************************************************
 *		AnyPopup (USER32.@)
 */
BOOL WINAPI AnyPopup(void)
{
    int i;
    BOOL retvalue;
    HWND *list = WIN_ListChildren( GetDesktopWindow() );

    if (!list) return FALSE;
    for (i = 0; list[i]; i++)
    {
        if (IsWindowVisible( list[i] ) && GetWindow( list[i], GW_OWNER )) break;
    }
    retvalue = (list[i] != 0);
    HeapFree( GetProcessHeap(), 0, list );
    return retvalue;
}


/*******************************************************************
 *		FlashWindow (USER32.@)
 */
BOOL WINAPI FlashWindow( HWND hWnd, BOOL bInvert )
{
    WND *wndPtr = WIN_FindWndPtr(hWnd);

    TRACE("%04x\n", hWnd);

    if (!wndPtr) return FALSE;
    hWnd = wndPtr->hwndSelf;  /* make it a full handle */

    if (wndPtr->dwStyle & WS_MINIMIZE)
    {
        if (bInvert && !(wndPtr->flags & WIN_NCACTIVATED))
        {
            HDC hDC = GetDC(hWnd);

            if (!SendMessageW( hWnd, WM_ERASEBKGND, (WPARAM16)hDC, 0 ))
                wndPtr->flags |= WIN_NEEDS_ERASEBKGND;

            ReleaseDC( hWnd, hDC );
            wndPtr->flags |= WIN_NCACTIVATED;
        }
        else
        {
            RedrawWindow( hWnd, 0, 0, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_FRAME );
            wndPtr->flags &= ~WIN_NCACTIVATED;
        }
        WIN_ReleaseWndPtr(wndPtr);
        return TRUE;
    }
    else
    {
        WPARAM16 wparam;
        if (bInvert) wparam = !(wndPtr->flags & WIN_NCACTIVATED);
        else wparam = (hWnd == GetActiveWindow());

        WIN_ReleaseWndPtr(wndPtr);
        SendMessageW( hWnd, WM_NCACTIVATE, wparam, (LPARAM)0 );
        return wparam;
    }
}


/*******************************************************************
 *		GetWindowContextHelpId (USER32.@)
 */
DWORD WINAPI GetWindowContextHelpId( HWND hwnd )
{
    DWORD retval;
    WND *wnd = WIN_FindWndPtr( hwnd );
    if (!wnd) return 0;
    retval = wnd->helpContext;
    WIN_ReleaseWndPtr(wnd);
    return retval;
}


/*******************************************************************
 *		SetWindowContextHelpId (USER32.@)
 */
BOOL WINAPI SetWindowContextHelpId( HWND hwnd, DWORD id )
{
    WND *wnd = WIN_FindWndPtr( hwnd );
    if (!wnd) return FALSE;
    wnd->helpContext = id;
    WIN_ReleaseWndPtr(wnd);
    return TRUE;
}


/*******************************************************************
 *			DRAG_QueryUpdate
 *
 * recursively find a child that contains spDragInfo->pt point
 * and send WM_QUERYDROPOBJECT
 */
BOOL16 DRAG_QueryUpdate( HWND hQueryWnd, SEGPTR spDragInfo, BOOL bNoSend )
{
    BOOL16 wParam, bResult = 0;
    POINT pt;
    LPDRAGINFO16 ptrDragInfo = MapSL(spDragInfo);
    RECT tempRect;

    if (!ptrDragInfo) return FALSE;

    CONV_POINT16TO32( &ptrDragInfo->pt, &pt );

    GetWindowRect(hQueryWnd,&tempRect);

    if( !PtInRect(&tempRect,pt) || !IsWindowEnabled(hQueryWnd)) return FALSE;

    if (!IsIconic( hQueryWnd ))
    {
        GetClientRect( hQueryWnd, &tempRect );
        MapWindowPoints( hQueryWnd, 0, (LPPOINT)&tempRect, 2 );

        if (PtInRect( &tempRect, pt))
        {
            int i;
            HWND *list = WIN_ListChildren( hQueryWnd );

            wParam = 0;

            if (list)
            {
                for (i = 0; list[i]; i++)
                {
                    if (GetWindowLongW( list[i], GWL_STYLE ) & WS_VISIBLE)
                    {
                        GetWindowRect( list[i], &tempRect );
                        if (PtInRect( &tempRect, pt )) break;
                    }
                }
                if (list[i])
                {
                    if (IsWindowEnabled( list[i] ))
                        bResult = DRAG_QueryUpdate( list[i], spDragInfo, bNoSend );
                }
                HeapFree( GetProcessHeap(), 0, list );
            }
            if(bResult) return bResult;
        }
        else wParam = 1;
    }
    else wParam = 1;

    ScreenToClient16(hQueryWnd,&ptrDragInfo->pt);

    ptrDragInfo->hScope = hQueryWnd;

    if (bNoSend) bResult = (GetWindowLongA( hQueryWnd, GWL_EXSTYLE ) & WS_EX_ACCEPTFILES) != 0;
    else bResult = SendMessage16( hQueryWnd, WM_QUERYDROPOBJECT, (WPARAM16)wParam, spDragInfo );

    if( !bResult ) CONV_POINT32TO16( &pt, &ptrDragInfo->pt );

    return bResult;
}


/*******************************************************************
 *		DragDetect (USER32.@)
 */
BOOL WINAPI DragDetect( HWND hWnd, POINT pt )
{
    MSG msg;
    RECT rect;

    rect.left = pt.x - wDragWidth;
    rect.right = pt.x + wDragWidth;

    rect.top = pt.y - wDragHeight;
    rect.bottom = pt.y + wDragHeight;

    SetCapture(hWnd);

    while(1)
    {
	while(PeekMessageA(&msg ,0 ,WM_MOUSEFIRST ,WM_MOUSELAST ,PM_REMOVE))
        {
            if( msg.message == WM_LBUTTONUP )
	    {
		ReleaseCapture();
		return 0;
            }
            if( msg.message == WM_MOUSEMOVE )
	    {
                POINT tmp;
                tmp.x = LOWORD(msg.lParam);
                tmp.y = HIWORD(msg.lParam);
		if( !PtInRect( &rect, tmp ))
                {
		    ReleaseCapture();
		    return 1;
                }
	    }
        }
	WaitMessage();
    }
    return 0;
}

/******************************************************************************
 *		DragObject (USER.464)
 */
DWORD WINAPI DragObject16( HWND16 hwndScope, HWND16 hWnd, UINT16 wObj,
                           HANDLE16 hOfStruct, WORD szList, HCURSOR16 hCursor )
{
    MSG	msg;
    LPDRAGINFO16 lpDragInfo;
    SEGPTR	spDragInfo;
    HCURSOR16 	hDragCursor=0, hOldCursor=0, hBummer=0;
    HGLOBAL16	hDragInfo  = GlobalAlloc16( GMEM_SHARE | GMEM_ZEROINIT, 2*sizeof(DRAGINFO16));
    HCURSOR16	hCurrentCursor = 0;
    HWND16	hCurrentWnd = 0;

    lpDragInfo = (LPDRAGINFO16) GlobalLock16(hDragInfo);
    spDragInfo = K32WOWGlobalLock16(hDragInfo);

    if( !lpDragInfo || !spDragInfo ) return 0L;

    if (!(hBummer = LoadCursorA(0, MAKEINTRESOURCEA(OCR_NO))))
    {
        GlobalFree16(hDragInfo);
        return 0L;
    }

    if(hCursor)
    {
	if( !(hDragCursor = CURSORICON_IconToCursor(hCursor, FALSE)) )
	{
	    GlobalFree16(hDragInfo);
	    return 0L;
	}

	if( hDragCursor == hCursor ) hDragCursor = 0;
	else hCursor = hDragCursor;

	hOldCursor = SetCursor(hDragCursor);
    }

    lpDragInfo->hWnd   = hWnd;
    lpDragInfo->hScope = 0;
    lpDragInfo->wFlags = wObj;
    lpDragInfo->hList  = szList; /* near pointer! */
    lpDragInfo->hOfStruct = hOfStruct;
    lpDragInfo->l = 0L;

    SetCapture(hWnd);
    ShowCursor( TRUE );

    do
    {
        GetMessageW( &msg, 0, WM_MOUSEFIRST, WM_MOUSELAST );

       *(lpDragInfo+1) = *lpDragInfo;

	lpDragInfo->pt.x = msg.pt.x;
	lpDragInfo->pt.y = msg.pt.y;

	/* update DRAGINFO struct */
	TRACE_(msg)("lpDI->hScope = %04x\n",lpDragInfo->hScope);

	if( DRAG_QueryUpdate(hwndScope, spDragInfo, FALSE) > 0 )
	    hCurrentCursor = hCursor;
	else
        {
            hCurrentCursor = hBummer;
            lpDragInfo->hScope = 0;
	}
	if( hCurrentCursor )
	    SetCursor(hCurrentCursor);

	/* send WM_DRAGLOOP */
	SendMessage16( hWnd, WM_DRAGLOOP, (WPARAM16)(hCurrentCursor != hBummer),
	                                  (LPARAM) spDragInfo );
	/* send WM_DRAGSELECT or WM_DRAGMOVE */
	if( hCurrentWnd != lpDragInfo->hScope )
	{
	    if( hCurrentWnd )
	        SendMessage16( hCurrentWnd, WM_DRAGSELECT, 0,
		       (LPARAM)MAKELONG(LOWORD(spDragInfo)+sizeof(DRAGINFO16),
				        HIWORD(spDragInfo)) );
	    hCurrentWnd = lpDragInfo->hScope;
	    if( hCurrentWnd )
                SendMessage16( hCurrentWnd, WM_DRAGSELECT, 1, (LPARAM)spDragInfo);
	}
	else
	    if( hCurrentWnd )
	        SendMessage16( hCurrentWnd, WM_DRAGMOVE, 0, (LPARAM)spDragInfo);

    } while( msg.message != WM_LBUTTONUP && msg.message != WM_NCLBUTTONUP );

    ReleaseCapture();
    ShowCursor( FALSE );

    if( hCursor )
    {
	SetCursor( hOldCursor );
	if (hDragCursor) DestroyCursor( hDragCursor );
    }

    if( hCurrentCursor != hBummer )
	msg.lParam = SendMessage16( lpDragInfo->hScope, WM_DROPOBJECT,
				   (WPARAM16)hWnd, (LPARAM)spDragInfo );
    else
        msg.lParam = 0;
    GlobalFree16(hDragInfo);

    return (DWORD)(msg.lParam);
}


/******************************************************************************
 *		GetWindowModuleFileNameA (USER32.@)
 */
UINT WINAPI GetWindowModuleFileNameA( HWND hwnd, LPSTR lpszFileName, UINT cchFileNameMax)
{
    FIXME("GetWindowModuleFileNameA(hwnd 0x%x, lpszFileName %p, cchFileNameMax %u) stub!\n",
          hwnd, lpszFileName, cchFileNameMax);
    return 0;
}

/******************************************************************************
 *		GetWindowModuleFileNameW (USER32.@)
 */
UINT WINAPI GetWindowModuleFileNameW( HWND hwnd, LPSTR lpszFileName, UINT cchFileNameMax)
{
    FIXME("GetWindowModuleFileNameW(hwnd 0x%x, lpszFileName %p, cchFileNameMax %u) stub!\n",
          hwnd, lpszFileName, cchFileNameMax);
    return 0;
}
