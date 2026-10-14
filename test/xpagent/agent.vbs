' ============================================================
'  XP Test Agent  -  polling command runner over HTTP
'  Runs on Windows XP / 7 inside the interactive RDP session.
'
'  Protocol:
'    GET  /cmd                  -> plain-text command (empty = nothing)
'    POST /result               -> command output
'    GET  /file/<name>          -> download file from host
'    POST /upload/<name>        -> upload file to host
'
'  Built-in host directives:
'    @DOWNLOAD <remote> <local>
'    @UPLOAD   <local> <remote>
'    @LOG                   -> upload the agent log
' ============================================================
Option Explicit

Dim HOST, PORT, BASE, WORK, LOGFILE
HOST = "192.168.1.116"
PORT = 8899
BASE = "http://" & HOST & ":" & PORT
WORK = "C:\xptest"
LOGFILE = WORK & "\agent.log"

Dim sh, fso
Dim g_curSeq
g_curSeq = ""
Set sh  = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
If Not fso.FolderExists(WORK) Then fso.CreateFolder WORK

Log "[boot] agent starting"
Boot

Sub Boot()
	Dim i, r
	r = "none"
	For i = 1 To 30
		r = HttpPost("/result", "[agent] alive " & Now() & " on " & sh.ExpandEnvironmentStrings("%COMPUTERNAME%"))
		If r = "ok" Then Exit For
		WScript.Sleep 1000
	Next
	Log "[boot] handshake=" & r
	MainLoop
End Sub

Sub MainLoop()
	Dim cmd, n
	n = 0
	Do
		Dim raw, bar
		raw = HttpPostText("/cmd", "poll")
		cmd = ""
		bar = InStr(raw, "|")
		If bar > 0 Then
			g_curSeq = Left(raw, bar - 1)
			cmd = Mid(raw, bar + 1)
		End If
		If Len(Trim(cmd)) > 0 Then
			Log "[cmd] " & Left(cmd, 300)
			Dispatch Trim(cmd)
			n = 0
		Else
			n = n + 1
			If n Mod 240 = 0 Then Log "[poll] alive " & Now()
			WScript.Sleep 250
		End If
	Loop
End Sub

Sub Dispatch(cmd)
	Dim p, a, b
	If Left(cmd, 10) = "@DOWNLOAD " Then
		p = Split(Mid(cmd, 11), " ")
		a = p(0)
		b = p(1)
		DownloadFile a, b
		HttpPost "/result", "[downloaded] " & a & " -> " & b
	ElseIf Left(cmd, 8) = "@UPLOAD " Then
		p = Split(Mid(cmd, 9), " ")
		a = p(0)
		b = p(1)
		HttpPost "/result", UploadFile(a, b)
	ElseIf Left(cmd, 7) = "@START " Then
		' launch something that must keep running (a GUI app, a recorder)
		' without blocking the agent: WshShell.Run with bWaitOnReturn=False
		Dim rc
		rc = ""
		On Error Resume Next
		sh.Run Mid(cmd, 8), 1, False
		If Err.Number <> 0 Then
			rc = "[error] @START failed: " & Err.Description
			Err.Clear
		Else
			rc = "[started] " & Mid(cmd, 8)
		End If
		On Error GoTo 0
		HttpPost "/result", rc
	ElseIf cmd = "@LOG" Then
		HttpPost "/result", UploadFile(LOGFILE, "agent.log")
	Else
		ExecCmd cmd
	End If
End Sub

' ---- run a shell command, capture output -------------------
' Output goes to a file (never a pipe) so a command that neither writes nor
' exits can never block the agent, and every command gets its OWN temporary
' files so a stale/read-only leftover can never break the next command.
Sub ExecCmd(cmd)
	Dim f, ps, out, bat, outf, donef, t0, pid, ts, tag, jobf

	tag = CStr(Int(Timer * 1000)) & CStr(Int(Rnd() * 100000))
	jobf  = WORK & "\job" & tag & ".bat"
	bat   = WORK & "\run" & tag & ".bat"
	outf  = WORK & "\out" & tag & ".txt"
	donef = WORK & "\done" & tag & ".txt"

	' the user command goes into its own file and the wrapper CALLs it:
	' a batch that invokes another batch without CALL never returns, which
	' would leave the completion marker unwritten.
	Set f = fso.CreateTextFile(jobf, True)
	f.WriteLine "@echo off"
	f.WriteLine "cd /d " & WORK
	f.WriteLine cmd
	f.Close

	Set f = fso.CreateTextFile(bat, True)
	f.WriteLine "@echo off"
	f.WriteLine "call " & jobf
	f.WriteLine "echo %ERRORLEVEL% > " & donef
	f.Close

	out = ""
	On Error Resume Next
	Set ps = sh.Exec("cmd.exe /d /c " & bat & " > " & outf & " 2>&1 < nul")
	If Err.Number <> 0 Then
		HttpPost "/result", "[error] cannot exec: " & Err.Description
		Err.Clear
		On Error GoTo 0
		Exit Sub
	End If
	pid = ps.ProcessID
	Err.Clear
	On Error GoTo 0

	t0 = Timer
	Do
		If fso.FileExists(donef) Then Exit Do
		If TimeElapsed(t0) > 300 Then
			On Error Resume Next
			sh.Run "taskkill /f /t /pid " & pid, 0, False
			Err.Clear
			On Error GoTo 0
			out = "[timeout] command killed after 300s" & vbCrLf
			Exit Do
		End If
		WScript.Sleep 100
	Loop

	On Error Resume Next
	If fso.FileExists(outf) Then
		Set ts = fso.OpenTextFile(outf, 1)
		If Not ts.AtEndOfStream Then out = out & ts.ReadAll()
		ts.Close
	End If
	If fso.FileExists(donef) Then
		Set ts = fso.OpenTextFile(donef, 1)
		If Not ts.AtEndOfStream Then out = out & "[exit " & Trim(ts.ReadAll()) & "]"
		ts.Close
	End If
	On Error GoTo 0

	' best effort cleanup; leftover temp files are harmless
	On Error Resume Next
	sh.Run "cmd.exe /d /c del /f /q """ & bat & """ """ & jobf & """ """ & outf & """ """ & donef & """", 0, False
	Err.Clear
	On Error GoTo 0

	HttpPost "/result", out
End Sub

Function TimeElapsed(t0)
	Dim t
	t = Timer
	If t < t0 Then t = t + 86400
	TimeElapsed = t - t0
End Function

' ---- HTTP helpers -----------------------------------------
' Prefer ServerXMLHTTP (supports timeouts), fall back to XMLHTTP.
Dim httpKind
httpKind = 0

Function NewHttp()
	Dim o
	On Error Resume Next
	Set o = CreateObject("MSXML2.ServerXMLHTTP")
	If Err.Number = 0 Then
		httpKind = 1
		Err.Clear
		On Error GoTo 0
		Set NewHttp = o
		Exit Function
	End If
	Err.Clear
	httpKind = 2
	Set o = CreateObject("MSXML2.XMLHTTP")
	On Error GoTo 0
	Set NewHttp = o
End Function

Sub SetTimeouts(xh)
	On Error Resume Next
	If httpKind = 1 Then xh.setTimeouts 4000, 4000, 8000, 8000
	Err.Clear
	On Error GoTo 0
End Sub

Function HttpGet(path)
	Dim xh
	HttpGet = ""
	On Error Resume Next
	Set xh = NewHttp()
	SetTimeouts xh
	xh.Open "GET", BASE & path, False
	xh.Send
	If Err.Number = 0 Then
		If xh.Status = 200 Then HttpGet = xh.responseText
	End If
	If Err.Number <> 0 Then Log "[err] GET " & path & " : " & Err.Description
	Err.Clear
	On Error GoTo 0
End Function

Function HttpPost(path, txt)
	Dim xh, i
	If path = "/result" And Len(g_curSeq) > 0 Then path = path & "?seq=" & g_curSeq
	HttpPost = "fail"
	For i = 1 To 4
		On Error Resume Next
		Set xh = NewHttp()
		SetTimeouts xh
		xh.Open "POST", BASE & path, False
		xh.setRequestHeader "Content-Type", "text/plain; charset=utf-8"
		xh.Send txt
		If Err.Number = 0 Then
			If xh.Status = 200 Then
				HttpPost = "ok"
				Err.Clear
				On Error GoTo 0
				Exit Function
			End If
		End If
		If Err.Number <> 0 Then Log "[err] POST " & path & " : " & Err.Description
		Err.Clear
		On Error GoTo 0
		WScript.Sleep 400
	Next
End Function

' POST that returns the response body (used for polling: POST is never cached)
Function HttpPostText(path, txt)
	Dim xh
	HttpPostText = ""
	On Error Resume Next
	Set xh = NewHttp()
	SetTimeouts xh
	xh.Open "POST", BASE & path, False
	xh.setRequestHeader "Content-Type", "text/plain; charset=utf-8"
	xh.Send txt
	If Err.Number = 0 Then
		If xh.Status = 200 Then HttpPostText = xh.responseText
	End If
	If Err.Number <> 0 Then Log "[err] POSTTEXT " & path & " : " & Err.Description
	Err.Clear
	On Error GoTo 0
End Function

Sub DownloadFile(remote, local)
	Dim xh, st
	On Error Resume Next
	Set xh = NewHttp()
	SetTimeouts xh
	xh.Open "GET", BASE & "/file/" & remote, False
	xh.Send
	If Err.Number <> 0 Or xh.Status <> 200 Then
		Log "[err] download " & remote & " : " & Err.Description
		Err.Clear
		On Error GoTo 0
		HttpPost "/result", "[error] download failed: " & remote
		Exit Sub
	End If
	Set st = CreateObject("ADODB.Stream")
	st.Type = 1
	st.Open
	st.Write xh.responseBody
	st.SaveToFile local, 2
	st.Close
	Err.Clear
	On Error GoTo 0
End Sub

Function UploadFile(local, remote)
	Dim xh, st, n
	UploadFile = "[error] upload failed: " & local
	If Not fso.FileExists(local) Then
		UploadFile = "[error] no such file: " & local
		Exit Function
	End If
	On Error Resume Next
	Set st = CreateObject("ADODB.Stream")
	st.Type = 1
	st.Open
	st.LoadFromFile local
	n = st.Size
	Set xh = NewHttp()
	SetTimeouts xh
	xh.Open "POST", BASE & "/upload/" & remote, False
	xh.setRequestHeader "Content-Type", "application/octet-stream"
	xh.Send st.Read
	st.Close
	If Err.Number = 0 Then
		If xh.Status = 200 Then UploadFile = "[uploaded] " & local & " (" & n & " bytes) -> " & remote
	End If
	If Err.Number <> 0 Then Log "[err] upload " & remote & " : " & Err.Description
	Err.Clear
	On Error GoTo 0
End Function

' ---- local log --------------------------------------------
Dim logCount
logCount = 0

Sub Log(s)
	On Error Resume Next
	logCount = logCount + 1
	Dim f
	Set f = fso.OpenTextFile(LOGFILE, 8, True)
	f.WriteLine Now() & " " & s
	f.Close
	If logCount > 4000 Then
		logCount = 0
		If fso.FileExists(LOGFILE & ".old") Then fso.DeleteFile LOGFILE & ".old", True
		fso.MoveFile LOGFILE, LOGFILE & ".old"
	End If
	Err.Clear
	On Error GoTo 0
End Sub
