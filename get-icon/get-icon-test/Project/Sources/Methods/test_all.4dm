//%attributes = {"invisible":true}
If (Application info.headless)
    var $state : Object
    var $process; $deadline : Integer
    test_icons
    $state:=New shared object("done"; False; "success"; False)
    $process:=New process("test_icons_process"; 0; "Icon cooperative test"; $state)
    $deadline:=Milliseconds+120000
    While (Not($state.done) & (Milliseconds<$deadline) & (Process state($process)>=0))
        DELAY PROCESS(Current process; 1)
    End while
    ASSERT($state.done & $state.success; "Icon extraction from a cooperative process")
    LOG EVENT(Into system standard outputs; "PASS"; Information message)
End if
