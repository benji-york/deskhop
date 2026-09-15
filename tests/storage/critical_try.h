#pragma once
/* Native boundary for the RP2040's one-read hardware lock acquisition. */
bool dh_critical_section_try_enter(critical_section_t *section);
