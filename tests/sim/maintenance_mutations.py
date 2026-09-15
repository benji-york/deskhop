"""Production maintenance mutations consumed by the shared paired runner.

Each mutation removes one safety contract and names an independent observable
oracle. The shared runner first proves every scenario passes unchanged source,
then requires the mutated firmware to compile and fail that runtime assertion.
Neither compilation failures nor an unrelated exception count as a kill.
"""

SOURCE_MUTATIONS = [
    (
        'maintenance-skip-local-usb-completion', 'maintenance.c',
        'bool reset = local_reply_complete && wire_idle();',
        'bool reset = wire_idle();',
        'serial_bootloader_local_0', 'stopped[0] on 0',
    ),
    (
        'maintenance-skip-ack-wire-drain', 'maintenance.c',
        'bool reset = server_phase == SERVER_DRAIN && server_ack_sent && wire_idle();',
        'bool reset = server_phase == SERVER_DRAIN && server_ack_sent;',
        'serial_bootloader_acceptance_not_boot', 'stopped[0] on 1',
    ),
    (
        'maintenance-send-cancelled-queued-request', 'uart.c',
        'if (!maintenance_packet_allowed(packet.type, packet.data, time_us_64()))',
        'if (false && !maintenance_packet_allowed(packet.type, packet.data, time_us_64()))',
        'serial_bootloader_source_cancel',
        'cancelled or expired maintenance request escaped to UART',
    ),
    (
        'maintenance-accept-dirty-target', 'maintenance.c',
        'global_state.fw.upgrade_in_progress || global_state.fw.image_dirty',
        'global_state.fw.upgrade_in_progress',
        'serial_bootloader_update_True_True', 'maintenance_outcome[0] on 0',
    ),
    (
        'maintenance-ignore-ack-token', 'maintenance.c',
        'memcmp(payload + 2, client_payload + 2, 6)',
        'memcmp(payload + 6, client_payload + 6, 2)',
        'serial_bootloader_malformed', 'maintenance_ready[0] on 0',
    ),
    (
        'maintenance-ignore-ack-session', 'maintenance.c',
        'memcmp(payload + 2, client_payload + 2, 6)',
        'memcmp(payload + 2, client_payload + 2, 4)',
        'serial_bootloader_old_session_ack', 'maintenance_ready[0] on 0',
    ),
]
