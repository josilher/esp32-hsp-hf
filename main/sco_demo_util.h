#ifndef SCO_DEMO_UTIL_H
#define SCO_DEMO_UTIL_H

#include "hci.h"

#if defined __cplusplus
extern "C"
{
#endif

  /**
 * @brief Init demo SCO data production/consumtion
 */
  void sco_demo_init(void);

  /**
 * @brief Set codec (cvsd:0x01, msbc:0x02) and initalize wav writter and portaudio .
 * @param codec
 */
  void sco_demo_set_codec(uint8_t codec);

  /**
 * @brief Send next data on con_handle
 * @param con_handle
 */
  void sco_demo_send(hci_con_handle_t con_handle);

  /**
 * @brief Process received data
 */
  void sco_demo_receive(uint8_t *packet, uint16_t size);

  /**
 * @brief Close WAV writer, stop portaudio stream
 */
  void sco_demo_close(void);

#if defined __cplusplus
}
#endif

#endif
