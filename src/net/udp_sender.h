#pragma once

// Initialise Wi-Fi in station mode and start the UDP sender task.
// The task consumes the global hid_event_queue and sends HidPackets
// to TARGET_IP:TARGET_PORT over UDP.
void udp_sender_init(void);
