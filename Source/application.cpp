/*
AutoHotkey

Copyright 2003 Chris Mallett

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "stdafx.h" // pre-compiled headers
#include "application.h"
#include "globaldata.h" // for access to g_clip, the "g" global struct, etc.
#include "window.h" // for serveral MsgBox and window functions
#include "util.h" // for strlcpy()
#include "resources\resource.h"  // For ID_TRAY_OPEN.


ResultType MsgSleep(int aSleepDuration, MessageMode aMode)
// Returns a non-meaningful value (so that it can return the result of something, thus
// effectively ignoring the result).  Callers should ignore it.  aSleepDuration can be
// zero to do a true Sleep(0), or less than 0 to avoid sleeping or waiting at all
// (i.e. messages are checked and if there are none, the function will return immediately).
// aMode is RETURN_AFTER_MESSAGES (default) or WAIT_FOR_MESSAGES.
// If the caller doesn't specify aSleepDuration, this function will return after a
// time less than or equal to SLEEP_INTERVAL (i.e. the exact amount of the sleep
// isn't important to the caller).  This mode is provided for performance reasons
// (it avoids calls to GetTickCount and the TickCount math).  However, if the
// caller's script subroutine is suspended due to action by us, an unknowable
// amount of time may pass prior to finally returning to the caller.
{
	// This is done here for performance reasons.  UPDATE: This probably never needs
	// to close the clipboard now that Line::ExecUntil() also calls CLOSE_CLIPBOARD_IF_OPEN:
	CLOSE_CLIPBOARD_IF_OPEN;
	// I know of no way to simulate a Sleep(0), so for now we do this.
	// UPDATE: It's more urgent that messages be checked than for the Sleep
	// duration to be zero, so for now, just let zero be handled like any other.
	//if (aMode == RETURN_AFTER_MESSAGES && aSleepDuration == 0)
	//{
	//	Sleep(0);
	//	return OK;
	//}
	// While in mode RETURN_AFTER_MESSAGES, there are different things that can happen:
	// 1) We launch a new hotkey subroutine, interrupting/suspending the old one.  But
	//    subroutine calls this function again, so now it's recursed.  And thus the
	//    new subroutine can be interrupted yet again.
	// 2) We launch a new hotkey subroutine, but it returns before any recursed call
	//    to this function discovers yet another hotkey waiting in the queue.  In this
	//    case, this instance/recursion layer of the function should process the
	//    hotkey messages linearly rather than recursively?  No, this doesn't seem
	//    necessary, because we can just return from our instance/layer and let the
	//    caller handle any messages waiting in the queue.  Eventually, the queue
	//    should be emptied, especially since most hotkey subroutines will run
	//    much faster than the user could press another hotkey, with the possible
	//    exception of the key-repeat feature triggered by holding a key down.
	//    Even in that case, the worst that would happen is that messages would
	//    get dropped off the queue because they're too old (I think that's what
	//    happens).
	// Based on the above, when mode is RETURN_AFTER_MESSAGES, we process
	// all messages until a hotkey message is encountered, at which time we
	// launch that subroutine only and then return when it returns to us, letting
	// the caller handle any additional messages waiting on the queue.  This avoids
	// the need to have a "run the hotkeys linearly" mode in a single iteration/layer
	// of this function.  Note: The WM_QUIT message does not receive any higher
	// precedence in the queue than other messages.  Thus, if there's ever concern
	// that that message would be lost, as a future change perhaps can use PeekMessage()
	// with a filter to explicitly check to see if our queue has a WM_QUIT in it
	// somewhere, prior to processing any messages that might take result in
	// a long delay before the remainder of the queue items are processed (there probably
	// aren't any such conditions now, so nothing to worry about?)

	// Above is somewhat out-of-date.  The objective now is to spend as much time
	// inside GetMessage() as possible, since it's the keystroke/mouse engine
	// whenever the hooks are installed.  Any time we're not in GetMessage() for
	// any length of time (say, more than 20ms), keystrokes and mouse events
	// will be lagged.  PeekMessage() is probably almost as good, but it probably
	// only clears out any waiting keys prior to returning.  CONFIRMED: PeekMessage()
	// definitely routes to the hook, perhaps only if called regularly (i.e. a single
	// isolated call might not help much).

	// This var allows us to suspend the currently-running subroutine and run any
	// hotkey events waiting in the message queue (if there are more than one, they
	// will be executed in sequence prior to resuming the suspended subroutine).
	// Never static because we could be recursed (e.g. when one hotkey iterruptes
	// a hotkey that has already been interrupted) and each recursion layer should
	// have it's own value for this?:
	global_struct global_saved;

	// Decided to support a true Sleep(0) for aSleepDuration == 0, as well
	// as no delay at all if aSleepDuration < 0.  This is needed to implement
	// "SetKeyDelay, 0" and possibly other things.  I believe a Sleep(0)
	// is always <= Sleep(1) because both of these will wind up waiting
	// a full timeslice if the CPU is busy.

	// Reminder for anyone maintaining or revising this code:
	// Giving each subroutine its own thread rather than suspending old ones is
	// probably not a good idea due to the exclusive nature of the GUI
	// (i.e. it's probably better to suspend existing subroutines rather than
	// letting them continue to run because they might activate windows and do
	// other stuff that would interfere with the GUI activities of other threads)

	// If caller didn't specify, the exact amount of the Sleep() isn't
	// critical to it, only that we handles messages and do Sleep()
	// a little.
	// Most of this initialization section isn't needed if aMode == WAIT_FOR_MESSAGES,
	// but it's done anyway for consistency:
	bool allow_early_return;
	if (aSleepDuration == INTERVAL_UNSPECIFIED)
	{
		aSleepDuration = SLEEP_INTERVAL;  // Set interval to be the default length.
		allow_early_return = true;
	}
	else
		// The timer resolution makes waiting for half or less of an
		// interval too chancy.  The correct thing to do on average
		// is some kind of rounding, which this helps with:
		allow_early_return = (aSleepDuration <= SLEEP_INTERVAL_HALF);

	// Record the start time when the caller first called us so we can keep
	// track of how much time remains to sleep (in case the caller's subroutine
	// is suspended until a new subroutine is finished).  But for small sleep
	// intervals, don't worry about it.
	// Note: QueryPerformanceCounter() has very high overhead compared to GetTickCount():
	DWORD start_time = allow_early_return ? 0 : GetTickCount();

	// This check is also done even if the main timer will be set (below) so that
	// an initial check is done rather than waiting 10ms more for the first timer
	// message to come in.  Some of our many callers would want this, and although some
	// would not need it, there are so many callers that it seems best to just do it
	// unconditionally, especially since it's not a high overhead call (e.g. it returns
	// immediately if the tickcount is still the same as when it was last run).
	// Another reason for doing this check immediately is that our msg queue might
	// contains a time-consuming msg prior to our WM_TIMER msg, e.g. a hotkey msg.
	// In that case, the hotkey would be processed and launched without us first having
	// emptied the queue to discover the WM_TIMER msg.  In other words, WM_TIMER msgs
	// might get buried in the queue behind others, so doing this check here should help
	// ensure that timed subroutines are checked often enough to keep them running at
	// their specified frequencies.
	// Note that ExecUntil() no longer needs to call us solely for prevention of lag
	// caused by the keyboard & mouse hooks, so checking the timers early, rather than
	// immediately going into the GetMessage() state, should not be a problem:
	CHECK_SCRIPT_TIMERS_IF_NEEDED

	// Because this function is called recursively: for now, no attempt is
	// made to improve performance by setting the timer interval to be
	// aSleepDuration rather than a standard short interval.  That would cause
	// a problem if this instance of the function invoked a new subroutine,
	// suspending the one that called this instance.  The new subroutine
	// might need a timer of a longer interval, which would mess up
	// this layer.  One solution worth investigating is to give every
	// layer/instance its own timer (the ID of the timer can be determined
	// from info in the WM_TIMER message).  But that can be a real mess
	// because what if a deeper recursion level receives our first
	// WM_TIMER message because we were suspended too long?  Perhaps in
	// that case we wouldn't our WM_TIMER pulse because upon returning
	// from those deeper layers, we would check to see if the current
	// time is beyond our finish time.  In addition, having more timers
	// might be worse for overall system performance than having a single
	// timer that pulses very frequently (because the system must keep
	// them all up-to-date).  UPDATE: Timer is now also needed whenever an
	// aSleepDuration greater than 0 is about to be done and there are some
	// script timers that need to be watched (this happens when aMode == WAIT_FOR_MESSAGES).
	// UPDATE: Make this a macro so that it is dynamically resolved every time, in case
	// the value of g_script.mTimerEnabledCount changes on-the-fly.
	// UPDATE #2: The below has been changed in light of the fact that the main timer is
	// now kept always-on whenever there is at least one enabled timed subroutine.
	// This policy simplifies ExecUntil() and long-running commands such as FileSetAttrib:
	bool we_turned_on_main_timer = aSleepDuration > 0 && aMode == RETURN_AFTER_MESSAGES && !g_MainTimerExists;
	if (we_turned_on_main_timer)
		SET_MAIN_TIMER
	// Even if the above didn't turn on the timer, it may already be on due to g_script.mTimerEnabledCount
	// being greater than zero.

	// Only used when aMode == RETURN_AFTER_MESSAGES:
	// True if the current subroutine was interrupted by another:
	//bool was_interrupted = false;
	bool sleep0_was_done = false;
	bool empty_the_queue_via_peek = false;

	HWND fore_window;
	DWORD fore_pid;
	char fore_class_name[32];
	MSG msg;

	for (;;)
	{
		if (aSleepDuration > 0 && !empty_the_queue_via_peek)
		{
			// Use GetMessage() whenever possible -- rather than PeekMessage() or a technique such
			// MsgWaitForMultipleObjects() -- because it's the "engine" that passes all keyboard
			// and mouse events immediately to the low-level keyboard and mouse hooks
			// (if they're installed).  Otherwise, there's greater risk of keyboard/mouse lag.
			// PeekMessage(), depending on how, and how often it's called, will also do this, but
			// I'm not as confident in it.
			if (GetMessage(&msg, NULL, 0, MSG_FILTER_MAX) == -1) // -1 is an error, 0 means WM_QUIT
			{
				// This probably can't happen since the above GetMessage() is getting any
				// message belonging to a thread we already know exists (i.e. the one we're
				// in now).
				//MsgBox("GetMessage() unexpectedly returned an error.  Press OK to continue running.");
				continue;
			}
			// else let any WM_QUIT be handled below.
		}
		else
		{
			// aSleepDuration <= 0 or "empty_the_queue_via_peek" is true, so we don't want
			// to be stuck in GetMessage() for even 10ms:
			if (!PeekMessage(&msg, NULL, 0, MSG_FILTER_MAX, PM_REMOVE)) // No more messages
			{
				// Since we're here, it means this recursion layer/instance of this
				// function didn't encounter any hotkey messages because if it had,
				// it would have already returned due to the WM_HOTKEY cases below.
				// So there should be no need to restore the value of global variables?
				if (aSleepDuration == 0 && !sleep0_was_done)
				{
					// Support a true Sleep(0) because it's the only way to yield
					// CPU time in this exact way.  It's used for things like
					// "SetKeyDelay, 0", which is defined as a Sleep(0) between
					// every keystroke
					// Out msg queue is empty, so do the sleep now, which might
					// yield the rest of our entire timeslice (probably 20ms
					// since we likely haven't used much of it) if the CPU is
					// under load:
					Sleep(0);
					sleep0_was_done = true;
					// Now start a new iteration of the loop that will see if we
					// received any messages during the up-to-20ms delay (perhaps even more)
					// that just occurred.  It's done this way to minimize keyboard/mouse
					// lag (if the hooks are installed) that will occur if any key or
					// mouse events are generated during that 20ms:
					continue;
				}
				else // aSleepDuration is non-zero or we already did the Sleep(0)
				{
					// Function should always return OK in this case.  Also, was_interrupted
					// will always be false because if this "aSleepDuration <= 0" call
					// really was interrupted, it would already have returned in the
					// hotkey cases of the switch().  UPDATE: was_interrupted can now
					// be true since the hotkey case in the switch() doesn't return,
					// relying on us to do it after making sure the queue is empty:
					return IsCycleComplete(aSleepDuration, start_time, allow_early_return);
				}
			}
			// else Peek() found a message, so process it below.
		}

		switch(msg.message)
		{
		case WM_QUIT:
			// Any other cleanup needed before this?  If the app owns any windows,
			// they're cleanly destroyed upon termination?
			// Note: If PostQuitMessage() was called to generate this message,
			// no new dialogs (e.g. MessageBox) can be created (perhaps no new
			// windows of any kind):
			g_script.ExitApp();
			continue;
		case WM_TIMER:
			if (msg.lParam) // Since this WM_TIMER is intended for a TimerProc, dispatch the msg instead.
				break;
			CHECK_SCRIPT_TIMERS_IF_NEEDED
			if (aMode == WAIT_FOR_MESSAGES)
				// Timer should have already been killed if we're in this state.
				// But there might have been some WM_TIMER msgs already in the queue
				// (they aren't removed when the timer is killed).  Or perhaps
				// a caller is calling us with this aMode even though there
				// are suspended subroutines (currently never happens).
				// In any case, these are always ignored in this mode because
				// the caller doesn't want us to ever return.  UPDATE: This can now
				// happen if there are any enabled timed subroutines we need to keep an
				// eye on, which is why the mTimerEnabledCount value is checked above
				// prior to starting a new iteration.
				continue;
			if (aSleepDuration <= 0) // In this case, WM_TIMER messages have already fulfilled their function, above.
				continue;
			// Otherwise aMode == RETURN_AFTER_MESSAGES:
			// Realistically, there shouldn't be any more messages in our queue
			// right now because the queue was stripped of WM_TIMER messages
			// prior to the start of the loop, which means this WM_TIMER
			// came in after the loop started.  So in the vast majority of
			// cases, the loop would have had enough time to empty the queue
			// prior to this message being received.  Therefore, just return rather
			// than trying to do one final iteration in peek-mode (which might
			// complicate things, i.e. the below function makes certain changes
			// in preparation for ending this instance/layer, only to be possibly,
			// but extremely rarely, interrupted/recursed yet again if that final
			// peek were to detect a recursable message):
			if (IsCycleComplete(aSleepDuration, start_time, allow_early_return))
				return OK;
			// Otherwise, stay in the blessed GetMessage() state until
			// the time has expired:
			continue;

		case WM_HOTKEY: // As a result of this app having previously called RegisterHotkey().
		case AHK_HOOK_HOTKEY:  // Sent from this app's keyboard or mouse hook.
			// MSG_FILTER_MAX should prevent us from receiving these messages whenever
			// g_AllowInterruption or g_AllowInterruptionForSub is false.
			if (g_nThreads >= g_MaxThreadsTotal
				&& !(ACT_IS_ALWAYS_ALLOWED(Hotkey::GetTypeOfFirstLine((HotkeyIDType)msg.wParam))
					&& g_nThreads < MAX_THREADS_LIMIT)   )
				// Allow only a limited number of recursion levels to avoid any chance of
				// stack overflow.  So ignore this message.  Later, can devise some way
				// to support "queuing up" these hotkey firings for use later when there
				// is "room" to run them, but that might cause complications because in
				// some cases, the user didn't intend to hit the key twice (e.g. due to
				// "fat fingers") and would have preferred to have it ignored.  Doing such
				// might also make "infinite key loops" harder to catch because the rate
				// of incoming hotkeys would be slowed down to prevent the subroutines from
				// running concurrently.
				continue;
				// It seems best not to buffer the key in the above case, since it might be a
				// while before the number of threads drops low enough.

			// Due to the key-repeat feature and the fact that most scripts use a value of 1
			// for their #MaxThreadsPerHotkey, this check will often help average performance
			// by avoiding a lot of unncessary overhead that would otherwise occur:
			if (!Hotkey::PerformIsAllowed((HotkeyIDType)msg.wParam))
			{
				// The key is buffered in this case to boost the responsiveness of hotkeys
				// that are being held down by the user to activate the keyboard's key-repeat
				// feature.  This way, there will always be one extra event waiting in the queue,
				// which will be fired almost the instant the previous iteration of the subroutine
				// finishes (this above descript applies only when MaxThreadsPerHotkey is 1,
				// which it usually is).
				Hotkey::RunAgainAfterFinished((HotkeyIDType)msg.wParam);
				continue;
			}

			// Always kill the main timer, for performance reasons and for simplicity of design,
			// prior to embarking on new subroutine whose duration may be long (e.g. if BatchLines
			// is very high or infinite, the called subroutine may not return to us for seconds,
			// minutes, or more; during which time we don't want the timer running because it will
			// only fill up the queue with WM_TIMER messages and thus hurt performance).
			// UPDATE: But don't kill it if it should be always-on to support the existence of
			// at least one enabled timed subroutine:
			if (!g_script.mTimerEnabledCount)
				KILL_MAIN_TIMER;

			if (aMode == RETURN_AFTER_MESSAGES)
			{
				// Assert: g_nThreads should be greater than 0 in this mode, which means
				// that there must be another thread besides the one we're about to create.
				// That thread will be interrupted and suspended to allow this new one to run.
				//was_interrupted = true;
				// Save the current foreground window in case the subroutine that's about
				// to be suspended is working with it.  Then, when the subroutine is
				// resumed, we can ensure this window is the foreground one.  UPDATE:
				// this has been disabled because it often is the incorrect thing to do
				// (e.g. if the suspended hotkey wasn't working with the window, but is
				// a long-running subroutine, hotkeys that activate windows will have
				// those windows deactivated instantly when their subroutine is over,
				// since the suspended subroutine resumes and would reassert its foreground
				// window:
				//g.hWndToRestore = aRestoreActiveWindow ? GetForegroundWindow() : NULL;

				// Also save the ErrorLevel of the subroutine that's about to be suspended.
				// Current limitation: If the user put something big in ErrorLevel (very unlikely
				// given its nature, but allowed) it will be truncated by this, if too large.
				// Also: Don't use var->Get() because need better control over the size:
				strlcpy(g.ErrorLevel, g_ErrorLevel->Contents(), sizeof(g.ErrorLevel));
				// Could also use copy constructor but that would probably incur more overhead?:
				CopyMemory(&global_saved, &g, sizeof(global_struct));
				// Next, change the value of globals to reflect the fact that we're about
				// to launch a new subroutine.
			}

			// Make every newly launched subroutine start off with the global default values that
			// the user set up in the auto-execute part of the script (e.g. KeyDelay, WinDelay, etc.).
			// However, we do not set ErrorLevel to NONE here because it's more flexible that way
			// (i.e. the user may want one hotkey subroutine to use the value of ErrorLevel set by another):
			CopyMemory(&g, &g_default, sizeof(global_struct));

			// Just prior to launching the hotkey, update these values to support built-in
			// variables such as A_TimeSincePriorHotkey:
			g_script.mPriorHotkeyLabel = g_script.mThisHotkeyLabel;
			g_script.mPriorHotkeyStartTime = g_script.mThisHotkeyStartTime;
			g_script.mThisHotkeyLabel = Hotkey::GetLabel((HotkeyIDType)msg.wParam);

			// If the current quasi-thread is paused, the thread we're about to launch
			// will not be, so the icon needs to be checked:
			g_script.UpdateTrayIcon();

			ENABLE_UNINTERRUPTIBLE_SUB

			// Do this last, right before the PerformID():
			// It seems best to reset these unconditionally, because the user has pressed a hotkey
			// so would expect maximum responsiveness, rather than the risk that a "rest" will be
			// done immediately by ExecUntil() just because mLinesExecutedThisCycle happens to be
			// large some prior subroutine.  The same applies to mLastScriptRest, which is why
			// that is reset also:
			g_script.mLinesExecutedThisCycle = 0;
			g_script.mThisHotkeyStartTime = g_script.mLastScriptRest = GetTickCount();

			// Perform the new hotkey's subroutine:
			++g_nThreads;
			Hotkey::PerformID((HotkeyIDType)msg.wParam);
			--g_nThreads;

			DISABLE_UNINTERRUPTIBLE_SUB
			g_LastPerformedHotkeyType = Hotkey::GetType((HotkeyIDType)msg.wParam); // For use with the KeyHistory cmd.

			if (aMode == RETURN_AFTER_MESSAGES)
			{
				// The unpause logic is done immediately after the most recently suspended thread's
				// global settings are restored so that that thread is set up properly to be resumed.
				// Comments about macro:
				//    g_UnpauseWhenResumed = false --> because we've "used up" this unpause ticket.
				//    g_ErrorLevel->Assign(g.ErrorLevel) --> restores the variable from the stored value.
				// If the thread to be resumed has not been unpaused, it will automatically be resumed in
				// a paused state because when we return from this function, we should be returning to
				// an instance of ExecUntil() (our caller), which should be in a pause loop still.
				// But always update the tray icon in case the paused state of the subroutine
				// we're about to resume is different from our previous paused state.  Do this even
				// when the macro is used by CheckScriptTimers(), which although it might not techically
				// need it, lends maintainability and peace of mind:
				#define RESUME_UNDERLYING_THREAD \
					CopyMemory(&g, &global_saved, sizeof(global_struct));\
					g_ErrorLevel->Assign(g.ErrorLevel);\
					if (g_UnpauseWhenResumed && g.IsPaused)\
					{\
						g_UnpauseWhenResumed = false;\
						g.IsPaused = false;\
						--g_nPausedThreads;\
					}\
					g_script.UpdateTrayIcon();
				RESUME_UNDERLYING_THREAD

				if (IsCycleComplete(aSleepDuration, start_time, allow_early_return))
				{
					// Check for messages once more in case the subroutine that just completed
					// above didn't check them that recently.  This is done to minimize the time
					// our thread spends *not* pumping messages, which in turn minimizes keyboard
					// and mouse lag if the hooks are installed.  Set the state of this function
					// layer/instance so that it will use peek-mode.  UPDATE: Don't change the
					// value of aSleepDuration to -1 because IsCycleComplete() needs to know the
					// original sleep time specified by the caller to determine whether
					// to restore the caller's active window after the caller's subroutine
					// is resumed-after-suspension:
					empty_the_queue_via_peek = true;
					allow_early_return = true;
					continue;  // i.e. end the switch() and have the loop do another iteration.
				}
				if (we_turned_on_main_timer) // Ensure the timer is back on since we still need it here.
					SET_MAIN_TIMER // This won't do anything if it's already on.
					// and stay in the blessed GetMessage() state until the time has expired.
			}
			continue;

#ifdef _DEBUG
		case AHK_HOOK_TEST_MSG:
		{
			char dlg_text[512];
			snprintf(dlg_text, sizeof(dlg_text), "TEST MSG: %d (0x%X)  %d (0x%X)"
				"\nCurrent Thread: 0x%X"
				, msg.wParam, msg.wParam, msg.lParam, msg.lParam
				, GetCurrentThreadId());
			MsgBox(dlg_text);
			continue;
		}
#endif

		case WM_KEYDOWN:
			if (msg.hwnd == g_hWndEdit && msg.wParam == VK_ESCAPE)
			{
				// This won't work if a MessageBox() window is displayed because its own internal
				// message pump will dispatch the message to our edit control, which will just
				// ignore it.  And avoiding setting the focus to the edit control won't work either
				// because the user can simply click in the window to set the focus.  But for now,
				// this is better than nothing:
				ShowWindow(g_hWnd, SW_HIDE);  // And it's okay if this msg gets dispatched also.
				continue;
			}
			// Otherwise, break so that the messages will get dispatched.  We need the other
			// WM_KEYDOWN msgs to be dispatched so that the cursor is keyboard-controllable in
			// the edit window:
			break;
		} // switch()

		// If a "continue" statement wasn't encountered somewhere in the switch(), we want to
		// process this message in a more generic way:
		// This little part is from the Miranda source code.  But it doesn't seem
		// to provide any additional functionality: You still can't use keyboard
		// keys to navigate in the dialog unless it's the topmost dialog.
		// UPDATE: The reason it doens't work for non-topmost MessageBoxes is that
		// this message pump isn't even the one running.  It's the pump of the
		// top-most MessageBox itself, which apparently doesn't properly dispatch
		// all types of messages to other MessagesBoxes.  However, keeping this
		// here is probably a good idea because testing reveals that it does
		// sometimes receive messages intended for MessageBox windows (which makes
		// sense because our message pump here retrieves all thread messages).
		// It might cause problems to dispatch such messages directly, since
		// IsDialogMessage() is supposed to be used in lieu of DispatchMessage()
		// for these types of messages:
		if ((fore_window = GetForegroundWindow()) != NULL)  // There is a foreground window.
		{
			GetWindowThreadProcessId(fore_window, &fore_pid);
			if (fore_pid == GetCurrentProcessId())  // It belongs to our process.
			{
				GetClassName(fore_window, fore_class_name, sizeof(fore_class_name));
				if (!strcmp(fore_class_name, "#32770"))  // MessageBox(), InputBox(), or FileSelectFile() window.
					if (IsDialogMessage(fore_window, &msg))  // This message is for it, so let it process it.
						continue;  // This message is done, so start a new iteration to get another msg.
			}
		}
		// Translate keyboard input for any of our thread's windows that need it:
		if (!g_hAccelTable || !TranslateAccelerator(g_hWnd, g_hAccelTable, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg); // This is needed to send keyboard input to various windows and for some WM_TIMERs.
		}
	} // infinite-loop
}



ResultType IsCycleComplete(int aSleepDuration, DWORD aStartTime, bool aAllowEarlyReturn)
// This function is used just to make MsgSleep() more readable/understandable.
{
	// Note: Even if TickCount has wrapped due to system being up more than about 49 days,
	// DWORD math still gives the right answer as long as aStartTime itself isn't more
	// than about 49 days ago. Note: must cast to int or any negative result will be lost
	// due to DWORD type:
	DWORD tick_now = GetTickCount();
	if (!aAllowEarlyReturn && (int)(aSleepDuration - (tick_now - aStartTime)) > SLEEP_INTERVAL_HALF)
		// Early return isn't allowed and the time remaining is large enough that we need to
		// wait some more (small amounts of remaining time can't be effectively waited for
		// due to the 10ms granularity limit of SetTimer):
		return FAIL; // Tell the caller to wait some more.

	// Reset counter for the caller of our caller, any time the thread
	// has had a chance to be idle (even if that idle time was done at a deeper
	// recursion level and not by this one), since the CPU will have been
	// given a rest, which is the main (perhaps only?) reason for using BatchLines
	// (e.g. to be more friendly toward time-critical apps such as games,
	// video capture, video playback).  UPDATE: mLastScriptRest is also reset
	// here because it has a very similar purpose.
	if (aSleepDuration >= 0)
	{
		g_script.mLinesExecutedThisCycle = 0;
		g_script.mLastScriptRest = tick_now;
	}

	// Kill the timer only if we're about to return OK to the caller since the caller
	// would still need the timer if FAIL was returned above.  But don't kill it if
	// there are any enabled timed subroutines, because the current policy it to keep
	// the main timer always-on in those cases:
	if (!g_script.mTimerEnabledCount)
		KILL_MAIN_TIMER

	return OK;
}



void CheckScriptTimers()
// Caller should already have checked the value of g_script.mTimerEnabledCount to ensure it's
// greater than zero, since we don't check that here (for performance).
// This function will go through the list of timed subroutines only once and then return to its caller.
// It does it only once so that it won't keep a thread beneath it permanently suspended if the sum
// total of all timer durations is too large to be run at their specified frequencies.
// This function is allowed to be called reentrantly, which handles certain situations better:
// 1) A hotkey subroutine interrupted and "buried" one of the timer subroutines in the stack.
//    In this case, we don't want all the timers blocked just because that one is, so reentrant
//    calls from ExecUntil() are allowed, and they might discover other timers to run.
// 2) If the script is idle but one of the timers winds up taking a long time to execute (perhaps
//    it gets stuck in a long WinWait), we want a reentrant call (from MsgSleep() in this example)
//    to launch any other enabled timers concurrently with the first, so that they're not neglected
//    just because one of the timers happens to be long-running.
// Of course, it's up to the user to design timers so that they don't cause problems when they
// interrupted hotkey subroutines, or when they themselves are interrupted by hotkey subroutines
// or other timer subroutines.
{

	// When this is true, such as during a SendKeys() operation, it seems best not to launch any new
	// timed subroutines.  The reasons for this are similar to the reasons for not allowing hotkeys
	// to fire during such times.  Those reasons are discussed in other comments.  In addition,
	// it seems best as a policy not to allow timed subroutines to run while the script's current
	// quasi-thread is paused.  Doing so would make the tray icon flicker (were it even updated below,
	// which it currently isn't) and in any case is probably not what the user would want.  Most of the
	// time, the user would want all timed subroutines stopped while the current thread is paused.
	// And even if this weren't true, the confusion caused by the subroutines still running even when
	// the current thread is paused isn't worth defaulting to the opposite approach.  In the future,
	// and if there's demand, perhaps a config option can added that allows a different default behavior.
	// UPDATE: It seems slightly better (more consistent) to disallow all timed subroutines whenever
	// there is even one paused thread anywhere in the "stack":
	if (!INTERRUPTIBLE || g_nPausedThreads > 0 || g_nThreads >= g_MaxThreadsTotal)
		return; // Above: To be safe (prevent stack faults) don't allow max threads to be exceeded.

	ScriptTimer *timer;
	UINT n_ran_subroutines;
	DWORD tick_start;
	global_struct global_saved;

	// Note: It seems inconsequential if a subroutine that the below loop executes causes a
	// new timer to be added to the linked list while the loop is still enumerating the timers.

	for (n_ran_subroutines = 0, timer = g_script.mFirstTimer; timer != NULL; timer = timer->mNextTimer)
	{
		// Call GetTickCount() every time in case a previous iteration of the loop took a long
		// time to execute:
		if (timer->mEnabled && timer->mExistingThreads < 1
			&& (tick_start = GetTickCount()) - timer->mTimeLastRun >= (DWORD)timer->mPeriod)
		{
			if (!n_ran_subroutines)
			{
				// Since this is the first subroutine that will be launched during this call to
				// this function, we know it will wind up running at least one subroutine, so
				// certain changes are made:
				// Increment the count of quasi-threads only once because this instance of this
				// function will never create more than 1 thread (i.e. if there is more than one
				// enabled timer subroutine, the will always be run sequentially by this instance).
				// If g_nThreads is zero, incremented it will also effectively mark the script as
				// non-idle, the main consequence being that an otherwise-idle script can be paused
				// if the user happens to do it at the moment a timed subroutine is running, which
				// seems best since some timed subroutines might take a long time to run:
				++g_nThreads;

				// Next, save the current state of the globals so that they can be restored just prior
				// to returning to our caller:
				strlcpy(g.ErrorLevel, g_ErrorLevel->Contents(), sizeof(g.ErrorLevel)); // Save caller's errorlevel.
				CopyMemory(&global_saved, &g, sizeof(global_struct));
				// But never kill the main timer, since the mere fact that we're here means that
				// there's at least one enabled timed subroutine.  Though later, performance can
				// be optimized by killing it if there's exactly one enabled subroutine, or if
				// all the subroutines are already in a running state (due to being buried beneath
				// the current quasi-thread).  However, that might introduce unwanted complexity
				// in other places that would need to start up the timer again because we stopped it, etc.
			}

			// This should slightly increase the expectation that any short timed subroutine will
			// run all the way through to completion rather than being interrupted by the press of
			// a hotkey, and thus potentially buried in the stack:
			g_script.mLinesExecutedThisCycle = 0;

			// This next line is necessary in case a prior iteration of our loop invoked a different
			// timed subroutine that changed any of the global struct's values.  In other words, make
			// every newly launched subroutine start off with the global default values that
			// the user set up in the auto-execute part of the script (e.g. KeyDelay, WinDelay, etc.).
			// However, we do not set ErrorLevel to NONE here because it's more flexible that way
			// (i.e. the user may want one hotkey subroutine to use the value of ErrorLevel set by another):
			CopyMemory(&g, &g_default, sizeof(global_struct));

			ENABLE_UNINTERRUPTIBLE_SUB

			++timer->mExistingThreads;
			timer->mLabel->mJumpToLine->ExecUntil(UNTIL_RETURN, 0);
			--timer->mExistingThreads;

			DISABLE_UNINTERRUPTIBLE_SUB


			// Seems better to store the start time rather than the finish time, though it's clearly
			// debatable.  The reason is that it's sometimes more important to ensure that a given
			// timed subroutine is *begun* at the specified interval, rather than assuming that
			// the specified interval is the time between when the prior run finished and the new
			// one began.  This should make timers behave more consistently (i.e. how long a timed
			// subroutine takes to run SHOULD NOT affect its *apparent* frequency, which is number
			// of times per second or per minute that we actually attempt to run it):
			timer->mTimeLastRun = tick_start;
			++n_ran_subroutines;
		}
	}

	if (n_ran_subroutines) // Since at least one subroutine was run above, restore various values for our caller.
	{
		--g_nThreads;  // Since this instance of this function only had one thread in use at a time.
		RESUME_UNDERLYING_THREAD // For explanation, see comments where the macro is defined.
	}
}



VOID CALLBACK DialogTimeout(HWND hWnd, UINT uMsg, UINT idEvent, DWORD dwTime)
{
	// Unfortunately, it appears that MessageBox() will return zero rather
	// than AHK_TIMEOUT, specified below -- at least under WinXP.  This
	// makes it impossible to distinguish between a MessageBox() that's been
	// timed out (destroyed) by this function and one that couldn't be
	// created in the first place due to some other error.  But since
	// MessageBox() errors are rare, we assume that they timed out if
	// the MessageBox() returns 0:
	EndDialog(hWnd, AHK_TIMEOUT);
	KillTimer(hWnd, idEvent);
}



VOID CALLBACK AutoExecSectionTimeout(HWND hWnd, UINT uMsg, UINT idEvent, DWORD dwTime)
// See the comments in AutoHotkey.cpp for an explanation of this function.
{
	// Since this was called, it means the AutoExec section hasn't yet finished (otherwise
	// this timer would have been killed before we got here).  UPDATE: I don't think this is
	// necessarily true.  I think it's possible for the WM_TIMER msg (since even TimerProc()
	// timers use WM_TIMER msgs) to be still buffered in the queue even though its timer
	// has been killed (killing the timer does not auto-purge any pending messages for
	// that timer, and it is risky/problematic to try to do so manually).  Therefore, although
	// we kill the timer here, we also do a double check further below to make sure
	// the desired action hasn't already occurred.  Finally, the macro is used here because
	// it's possible that the timer has already been killed, so we don't want to risk any
	// problems that might arise from killing a non-existent timer (which this prevents):
	KILL_AUTOEXEC_TIMER

	// This is a double-check because it's possible for the WM_TIMER message to have
	// been received (thus calling this TimerProc() function) even though the timer
	// was already killed by AutoExecSection().  In that case, we don't want to update
	// the global defaults again because the g struct might have incorrect/unintended
	// values by now:
	if (!g_script.AutoExecSectionIsRunning)
		return;

	// Otherwise:
	CopyMemory(&g_default, &g, sizeof(global_struct));
	global_clear_state(&g_default);  // Only clear g_default, not g.
	// And since the AutoExecute is taking a long time (or might never complete), we now
	// allow interruptions such as hotkeys and timed subroutines. Use g_AllowInterruptionForSub
	// vs. g_AllowInterruption in case commands in the AutoExecute section need exclusive use
	// of g_AllowInterruption (i.e. they might change its value to false and then back to true,
	// which would interfere with our use of that var):
	g_AllowInterruptionForSub = true;
}



VOID CALLBACK UninteruptibleTimeout(HWND hWnd, UINT uMsg, UINT idEvent, DWORD dwTime)
{
	KILL_UNINTERRUPTIBLE_TIMER // Best to use the macro so that g_UninterruptibleTimerExists is reset to false.
	g_AllowInterruptionForSub = true;
}
