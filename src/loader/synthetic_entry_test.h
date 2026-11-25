#pragma once
namespace scarlett {
// Builds and invokes ONLY a generated minimal x64 PE64 program: real import
// binding, DIR64 relocation, TLS preparation/deferred callbacks and RX
// section protection. Never executes any external/title-provided PE code.
bool verify_synthetic_entry_fixture() noexcept;
} // namespace scarlett
