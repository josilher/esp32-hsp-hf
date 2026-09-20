#include "host_link_spp.h"

#include <string.h>
#include "btstack.h"
#include "esp_log.h"

static const char *TAG = "host_link_spp";

#define SPP_TX_BUFFER_SIZE 1024
#define SPP_SDP_RECORD_HANDLE 0x10002

static host_link_t          s_link;
static uint16_t             s_cid = 0;
static uint8_t              s_sdp_buffer[150];
static btstack_ring_buffer_t s_tx_rb;
static uint8_t              s_tx_storage[SPP_TX_BUFFER_SIZE];
static uint32_t             s_tx_dropped = 0;

/* Envía lo que haya pendiente, como mucho un paquete RFCOMM por evento. */
static void send_pending(void)
{
    if (!s_cid) return;
    uint32_t avail = btstack_ring_buffer_bytes_available(&s_tx_rb);
    if (avail == 0) return;

    uint16_t mtu = rfcomm_get_max_frame_size(s_cid);
    uint32_t chunk = avail < mtu ? avail : mtu;

    rfcomm_reserve_packet_buffer();
    uint8_t *out = rfcomm_get_outgoing_buffer();
    uint32_t n = 0;
    btstack_ring_buffer_read(&s_tx_rb, out, chunk, &n);
    rfcomm_send_prepared(s_cid, (uint16_t)n);

    if (btstack_ring_buffer_bytes_available(&s_tx_rb) > 0) {
        rfcomm_request_can_send_now_event(s_cid);
    }
}

static void spp_send(host_link_t *link, const uint8_t *data, size_t len)
{
    (void)link;
    if (!s_cid) return;
    if (btstack_ring_buffer_bytes_free(&s_tx_rb) < len) {
        s_tx_dropped++;
        ESP_LOGW(TAG, "buffer de salida lleno, %u bytes descartados", (unsigned)len);
        return;
    }
    btstack_ring_buffer_write(&s_tx_rb, (uint8_t *)data, (uint32_t)len);
    rfcomm_request_can_send_now_event(s_cid);
}

static bool spp_is_connected(host_link_t *link)
{
    (void)link;
    return s_cid != 0;
}

static const host_link_ops_t s_ops = {
    .name         = "spp",
    .send         = spp_send,
    .is_connected = spp_is_connected,
};

static void spp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    switch (packet_type) {
    case RFCOMM_DATA_PACKET:
        if (channel == s_cid) host_link_rx(&s_link, packet, size);
        break;

    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case RFCOMM_EVENT_INCOMING_CONNECTION: {
            uint16_t cid = rfcomm_event_incoming_connection_get_rfcomm_cid(packet);
            if (s_cid) {
                ESP_LOGW(TAG, "conexion entrante rechazada: ya hay una activa");
                rfcomm_decline_connection(cid);
            } else {
                rfcomm_accept_connection(cid);
            }
            break;
        }
        case RFCOMM_EVENT_CHANNEL_OPENED:
            if (rfcomm_event_channel_opened_get_status(packet) == ERROR_CODE_SUCCESS) {
                s_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
                btstack_ring_buffer_init(&s_tx_rb, s_tx_storage, sizeof(s_tx_storage));
                s_link.rx_len = 0;
                ESP_LOGI(TAG, "conectado, cid=0x%04x mtu=%u", s_cid, rfcomm_get_max_frame_size(s_cid));
            } else {
                ESP_LOGW(TAG, "fallo al abrir canal: 0x%02x", rfcomm_event_channel_opened_get_status(packet));
            }
            break;
        case RFCOMM_EVENT_CAN_SEND_NOW:
            send_pending();
            break;
        case RFCOMM_EVENT_CHANNEL_CLOSED:
            if (rfcomm_event_channel_closed_get_rfcomm_cid(packet) == s_cid) {
                s_cid = 0;
                ESP_LOGI(TAG, "desconectado");
            }
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}

void host_link_spp_init(uint8_t rfcomm_channel, const char *service_name)
{
    btstack_ring_buffer_init(&s_tx_rb, s_tx_storage, sizeof(s_tx_storage));

    rfcomm_register_service(spp_packet_handler, rfcomm_channel, 0xffff);

    memset(s_sdp_buffer, 0, sizeof(s_sdp_buffer));
    spp_create_sdp_record(s_sdp_buffer, SPP_SDP_RECORD_HANDLE, rfcomm_channel, service_name);
    sdp_register_service(s_sdp_buffer);

    host_link_register(&s_link, &s_ops, NULL);
    ESP_LOGI(TAG, "servicio '%s' en canal RFCOMM %u", service_name, rfcomm_channel);
}
