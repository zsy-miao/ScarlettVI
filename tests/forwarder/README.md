# Pure-source forwarded-export fixture

This fixture is compiled locally using MSVC and **does not contain game code**.
`ForwardApp.exe` imports `SvForward` from `ForwardBridge.dll`, which
forwards it to `FixtureBase.dll!SvOriginal`. The loader must find the
second local DLL through the export forwarder, map both DLLs, and bind the
import without running either DLL or the executable.

Run the generated executable through `ScarlettVI.exe map` (never through a
title execution mode). Generated EXEs, LIBs and DLLs stay in gitignored
`build/fixtures`, not in the repository.
