#pragma once

// ---------------------------------------------------------------
// PER-BOARD IDENTITY -- copy this file to node_identity.h and put a
// real key in it. node_identity.h is gitignored; this example is not.
//
// This key is what makes a node individually recognizable across
// reboots and reflashes. Without a fixed key, RNS::Identity() mints
// a new random identity every boot and the destination hash changes
// every time, so nothing can reliably tell "this is the same node."
//
// Generate a fresh one PER BOARD:
//   python3 -c "import secrets; print(secrets.token_hex(65))"
//
// Board A and Board B must have different keys, or they announce as
// the same identity and the mesh can't distinguish them.
//
// Treat these like private keys, because that's what they are. Never
// commit a real one, never paste one into a chat or an issue. If a
// key leaks, generate a new one and reflash that board.
// ---------------------------------------------------------------

#define NODE_PRIVATE_KEY_HEX "REPLACE_ME_WITH_A_FRESHLY_GENERATED_KEY"
