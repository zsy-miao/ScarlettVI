#pragma once
namespace scarlett {
// Builds and invokes ONLY a generated minimal x64 PE64 program: real import
// binding, DIR64 relocation, TLS preparation/deferred callbacks and RX
// section protection. Never executes any external/title-provided PE code.
bool verify_synthetic_entry_fixture() noexcept;
// Runs ONLY a second generated PE32+ DLL through explicit process attach/
// detach callbacks and tests per-thread host TLS ownership. No title code.
bool verify_synthetic_dll_lifecycle_fixture() noexcept;
} // namespace scarlett
