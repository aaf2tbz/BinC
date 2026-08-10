// The frontend must report the missing stage struct and recover without
// dereferencing the aborted function during AIR metadata emission.
UnknownStageOutput MainPS() : SV_Target0 {
    return 0;
}
