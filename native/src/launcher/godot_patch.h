#pragma once

#include "globals.h"

/* Build an external Godot resource patch pack next to the selected game.
   The original .pck/embedded pack is copied, then English .translation
   resources are replaced with text Translation resources generated from the
   local translation server. The original game files are not modified. */
int godot_prepare_patch_pack(const WCHAR *dir, WCHAR *out_pack, size_t cap);

/* Loose Godot projects (project.godot plus resource files on disk) must not be
   launched with a minimal --main-pack: Godot directory enumeration then sees
   the patch pack instead of the original resource tree. They use a sidecar
   runtime script instead. */
int godot_is_loose_project(const WCHAR *dir);
int godot_prepare_runtime_sidecar(const WCHAR *dir);

/* Promote a background-built patch pack from the previous run, if present.
   This is intentionally separate from prepare: games may keep the active pack
   open while running, so refreshes can be staged for the next launch. */
int godot_promote_staged_patch_pack(const WCHAR *dir);
