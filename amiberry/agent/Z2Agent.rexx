/* Z2Agent.rexx: runs AmigaDOS commands that the host leaves on the shared drive (docs/driver.md,
   "Remote shell"). Started from S:User-Startup; driven by sw/emu/amiberry/z2amiga.py on the host.
   Protocol, in DH0:Z2Agent/ (a host directory, so the host sees the same files):
     cmd    an AmigaDOS script, written by the host (it renames it into place when complete); each command
            line already sends its output to RAM:z2a.out (>>RAM:z2a.out after the command name), because
            the redirection of an Execute does not reach the commands of its script
     rc     the script's return code (of its last command, as Execute gives it), written before out
     out    the script's output; its appearance means the command is finished
   The agent deletes cmd before it runs it, polls twice a second and has no window, so it never blocks
   IPrefs from reopening Workbench. Its own files are in RAM:, not T: (SYS:T, a host directory).
   Every Copy has BUF=0: run from a script, AmigaOS 3.1.4 Copy keeps its 64 KB buffer allocated after each
   run (65,544 bytes lost per Copy), which made memory look lost between two sh calls (docs/driver.md). */
if ~show('L', 'rexxsupport.library') then call addlib('rexxsupport.library', 0, -30, 0)
dir = 'DH0:Z2Agent/'
address command
'MakeDir >NIL: DH0:Z2Agent'
'Delete >NIL: DH0:Z2Agent/cmd DH0:Z2Agent/rc DH0:Z2Agent/out QUIET'
call pragma('D', 'SYS:')
do forever
    if ~exists(dir'cmd') then do
        call delay(25)
        iterate
    end
    'Copy >NIL: DH0:Z2Agent/cmd RAM:z2a.cmd BUF=0'
    'Delete >NIL: DH0:Z2Agent/cmd QUIET'
    if open('o', 'RAM:z2a.out', 'W') then call close('o')
    'Execute RAM:z2a.cmd'
    r = rc
    'Copy >NIL: RAM:z2a.out DH0:Z2Agent/out.tmp BUF=0'
    if open('f', dir'rc', 'W') then do
        call writeln('f', r)
        call close('f')
    end
    'Rename >NIL: DH0:Z2Agent/out.tmp DH0:Z2Agent/out'
    'Delete >NIL: RAM:z2a.cmd RAM:z2a.out QUIET'
end
