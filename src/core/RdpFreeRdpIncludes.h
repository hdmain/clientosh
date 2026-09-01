#pragma once

#if defined(__has_include)
#  if __has_include(<freerdp/freerdp.h>)
#    include <freerdp/freerdp.h>
#    include <freerdp/client.h>
#    include <freerdp/client/cmdline.h>
#    include <freerdp/codec/color.h>
#    include <freerdp/event.h>
#    include <freerdp/gdi/gdi.h>
#    include <freerdp/input.h>
#    include <freerdp/scancode.h>
#    include <freerdp/channels/cliprdr.h>
#    include <freerdp/client/cliprdr.h>
#    include <winpr/wtypes.h>
#    include <winpr/synch.h>
#  else
#    error "FreeRDP headers not found (install freerdp3 or freerdp2 development package)"
#  endif
#else
#  include <freerdp/freerdp.h>
#  include <freerdp/client.h>
#  include <freerdp/client/cmdline.h>
#  include <freerdp/codec/color.h>
#  include <freerdp/event.h>
#  include <freerdp/gdi/gdi.h>
#  include <freerdp/input.h>
#  include <freerdp/scancode.h>
#  include <freerdp/channels/cliprdr.h>
#  include <freerdp/client/cliprdr.h>
#  include <winpr/wtypes.h>
#  include <winpr/synch.h>
#endif

#ifndef CLIENTOSH_FREERDP_VERSION
#  define CLIENTOSH_FREERDP_VERSION 0
#endif

#if !defined(freerdp_client_settings_parse_command_line_arguments) \
    && defined(freerdp_client_settings_parse_command_line)
#  define freerdp_client_settings_parse_command_line_arguments(settings, argc, argv, allowUnknown) \
    freerdp_client_settings_parse_command_line((settings), (argc), (argv))
#endif

#if !defined(RDP_SCANCODE_PAGE_UP) && defined(RDP_SCANCODE_PRIOR)
#  define RDP_SCANCODE_PAGE_UP RDP_SCANCODE_PRIOR
#endif
#if !defined(RDP_SCANCODE_PAGE_DOWN) && defined(RDP_SCANCODE_NEXT)
#  define RDP_SCANCODE_PAGE_DOWN RDP_SCANCODE_NEXT
#endif
#if !defined(PTR_FLAGS_RELEASE)
#  define PTR_FLAGS_RELEASE 0
#endif

#if !defined(freerdp_input_send_keyboard_event_ex)
#  define freerdp_input_send_keyboard_event_ex(input, down, repeat, scancode) \
    freerdp_input_send_keyboard_event(                                       \
        (input), (down) ? KBD_FLAGS_DOWN : KBD_FLAGS_RELEASE,                \
        static_cast<UINT8>((scancode)&0xFF))
#endif
