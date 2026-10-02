#pragma once
// Every host translation unit must execute assertions, including their side effects.
#ifdef NDEBUG
#error "CPB host regression tests require assertions; NDEBUG must not be defined"
#endif
