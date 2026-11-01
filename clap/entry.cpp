// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// The module's exported symbol; the plugin itself is in plugin.cpp.
#include "plugin.h"

extern "C" OSCAIRN_SFIZZ_EXPORT const void* oscairn_sfizz_keep_entry = &clap_entry;
