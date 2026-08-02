#ifndef MEGAFLASH_GUEST_ADDRS_H
#define MEGAFLASH_GUEST_ADDRS_H

/* pico2_debug MegaFlash guest addresses — overlay only. */
#include <stdint.h>

#define USB_GUEST_TUD_DEVICE_STATE  0x2000b7c4u /* _usbd_dev */
#define USB_GUEST_TUD_MOUNTED_OFF   1u
#define USB_GUEST_TUD_CTRL_BUSY_OFF 0x35u
#define USB_GUEST_TUD_CDC_BASE      0x2000b674u /* _cdcd_itf */
#define USB_GUEST_TUD_CDC_LINE_OFF  4u
#define USB_GUEST_TUD_CDC_TXFIFO    (USB_GUEST_TUD_CDC_BASE + 0x24u)
#define USB_GUEST_TUD_CDC_RXFIFO    (USB_GUEST_TUD_CDC_BASE + 0x10u)
#define USB_GUEST_TUD_CDC_RXBUF     (USB_GUEST_TUD_CDC_BASE + 0x38u)
#define USB_GUEST_TUD_CDC_TXBUF     (USB_GUEST_TUD_CDC_BASE + 0x78u)
#define USB_GUEST_STDIO_MUTEX       0x2000516cu /* print_mutex */
#define USB_GUEST_STDIO_USB_MUTEX   0x20058064u
#define USB_GUEST_STDIO_USB_DRIVER  0x200047d4u /* Pico SDK stdio_usb driver RAM */
#define USB_GUEST_STDIO_ACTIVE_DRV  0x2000d1bcu /* drivers chain head */
#define USB_GUEST_STDIO_USB_OUT     0x1000e8ecu /* stdio_usb_out_chars */
#define USB_GUEST_STDIO_USB_FLUSH   0x1000e664u /* stdio_usb_out_flush */
#define USB_GUEST_STDIO_USB_IN      0x1000e854u /* stdio_usb_in_chars */
#define USB_GUEST_USB_PUTCHAR       0x10003b58u /* usb_putchar */
#define USB_GUEST_USB_PUTRAW        0x10003b74u /* usb_putraw */
#define USB_GUEST_TUD_CDC_AVAILABLE 0x100101ccu /* tud_cdc_n_available */
#define USB_GUEST_TUD_CDC_READ      0x100101e4u /* tud_cdc_n_read */
#define USB_GUEST_STDIO_USB_AVAIL   0x1000e650u /* chars_available callback */
#define USB_GUEST_CHECK_PICOW_INITED  0x20061607u
#define USB_GUEST_CHECK_PICOW_RESULT  0x20061608u
#define USB_GUEST_HOST_RX_CAP 262144u
#define USB_GUEST_HOST_RX_USABLE (USB_GUEST_HOST_RX_CAP - 1u)
#define USB_GUEST_HOST_RX_XMODEM_AHEAD 8192u
#define USB_GUEST_STDIO_PUTSTRING   0x1000e1a0u  /* stdio_put_string: blx out_chars */
#define USB_GUEST_STDIO_PUTSTRING_FN 0x1000e154u /* stdio_put_string entry */
#define USB_GUEST_WRAP_PUTCHAR_CALL 0x1000e2a0u  /* __wrap_putchar: bl stdio_put_string */
#define USB_GUEST_WRAP_PUTS         0x1000e2aau  /* __wrap_puts */
#define USB_GUEST_WRAP_PUTS_CALL    0x1000e2bcu  /* __wrap_puts: bl stdio_put_string */
#define USB_GUEST_WRAP_PRINTF       0x1000e2eau  /* __wrap_printf entry */
#define USB_GUEST_USER_TERMINAL_DEVINFO 0x10005b1cu /* bl DeviceInfo */
#define USB_GUEST_PRINT_BANNER      0x100056fcu
#define USB_GUEST_GET_DEVICE_INFO   0x10005088u /* GetDeviceInfoString */
#define USB_GUEST_ASSERT_FUNC       0x1000e028u /* __assert_func */
#define USB_GUEST_CHECK_ALLOC       0x1000dee4u /* check_alloc — skip heap bounds under emu */
#define USB_GUEST_ENABLE_SPI0       0x10002940u /* enable_spi0 — clamp deviceNum for emu */
#define USB_GUEST_SPI_WR_RD_VENEER  0x10034d88u /* __spi_write_read_blocking_veneer */
#define USB_GUEST_SPI_READ_BLOCKING_V 0x10034e30u /* __spi_read_blocking_veneer */
#define USB_GUEST_ALARM_POOL_DEFAULT  0x1000bbf8u /* alarm_pool_get_default */
#define USB_GUEST_MULTICORE_LAUNCH    0x10000344u /* main: bl multicore_launch_core1 */
#define USB_GUEST_INIT_SPI_CALL         0x100002e2u /* main: bl InitSpi */
#define USB_GUEST_U2_INIT_CALL          0x10000328u /* main: bl U2_Init */
#define USB_GUEST_LOAD_ALL_CONFIGS      0x10005354u /* LoadAllConfigs */
#define USB_GUEST_SAVE_USER_SETTINGS    0x10005420u /* SaveUserSettings */
#define USB_GUEST_ENCRYPT_WRITE_CFG     0x10005294u /* EncryptWriteConfigToFlash */
#define USB_GUEST_TS_WRITE_SEC_REG      0x10002f1cu /* tsWriteSecurityRegister */
#define USB_GUEST_TS_READ_SEC_REG       0x10002e68u /* tsReadSecurityRegister */
#define USB_GUEST_GET_CONFIG_BYTE1      0x100054b8u /* GetConfigByte1 */
#define USB_GUEST_GET_CONFIG_BYTE2      0x100054d4u /* GetConfigByte2 */
#define USB_GUEST_SAVE_CONFIGS_CALL     0x10000348u /* main: bl SaveConfigs */
#define USB_GUEST_IS_APPLE_CONNECTED_CALL  0x10000338u /* main: bl IsAppleConnected */
#define USB_GUEST_IS_APPLE_CONNECTED_CALL2 0x10000408u
#define USB_GUEST_CHECK_PICOW_CALL    0x10000398u /* main: bl CheckPicoW */
#define USB_GUEST_CHECK_PICOW_CALL2   0x100003beu
#define USB_GUEST_CHECK_PICOW_FN      0x10004e08u
#define USB_GUEST_IS_APPLE_CONNECTED_FN 0x10004db0u
#define USB_GUEST_CORE0_LOOP_VENEER   0x10034cf8u
#define USB_GUEST_ENABLE_APPLE_RST    0x100002c0u
#define USB_GUEST_STDIO_USB_INIT_CALL1 0x100003c6u /* main: bl stdio_usb_init (PicoW) */
#define USB_GUEST_STDIO_USB_INIT_CALL2 0x10000486u
#define USB_GUEST_STDIO_USB_INIT_FN   0x1000e9c0u
#define USB_GUEST_INIT_PICOLED_CALL1  0x100003cau
#define USB_GUEST_INIT_PICOLED_CALL2  0x1000048au
#define USB_GUEST_STDIO_USB_CONNECTED 0x1000e848u
#define USB_GUEST_USB_WAIT_LOOP       0x100003deu /* PicoW USB wait before UserTerminal */
#define USB_GUEST_USB_CONN_LOOP       0x10000494u /* non-PicoW USB wait loop */
#define USB_GUEST_CLOCK_GET_HZ        0x1000c398u
#define USB_GUEST_SPI_GET_BAUDRATE    0x10011ff0u
#define USB_GUEST_AON_TIMER_GET_CAL   0x100117ecu /* aon_timer_get_time_calendar */
#define USB_GUEST_RTC_RUNNING_BSS     0x2006161eu /* rtcRunning (bool) */
#define USB_GUEST_UART_PUTC           0x1000e308u /* uart_putc — bridge to TCP under -uart-console */
#define USB_GUEST_MAIN_USB_LOOP       0x10000414u /* bl UserTerminal (PicoW USB path) */
#define USB_GUEST_USER_TERMINAL       0x10005b00u
#define USB_GUEST_USB_GETKEY_RET      0x10003c7cu /* usb_getkey epilogue */
#define USB_GUEST_USB_GETSTRING       0x10003c80u /* usb_getstring */
#define USB_GUEST_USB_GETCHAR_TIMEOUT 0x10003b84u /* usb_getchar_timeout_us */
#define USB_GUEST_USB_GETRAW_TIMEOUT  0x10003bc0u /* usb_getraw_timeout */
#define USB_GUEST_WRITE_BLOCK_IMAGE   0x100035b4u /* WriteBlockForImageTransfer */
#define USB_GUEST_CRC16_ALIGNED       0x100046f4u /* CRC16Aligned — DMA CRC fails under emu */
#define USB_GUEST_COPY_MEMORY         0x10004698u /* CopyMemory */
#define USB_GUEST_COPY_MEMORY_ALIGNED 0x2000147cu /* CopyMemoryAligned (RAM) */
#define USB_GUEST_COPY_MEMORY_ALIGNED_V 0x10034d60u /* __CopyMemoryAligned_veneer */
#define USB_GUEST_XMODEM_LED_ON       0x10004004u /* PacketReceived: ActLed mcr + PicoLed */
#define USB_GUEST_XMODEM_WRITE_READY  0x10003ffau /* PacketReceived: blockNum load before write */
#define USB_GUEST_XMODEM_PARTS_STORED 0x10003feau /* PacketReceived: str partsAlreadyInBuffer */
#define USB_GUEST_XMODEM_COPY_LEN     0x10003fd2u /* PacketReceived: lsl r8,r5,#7 before copy */
#define USB_GUEST_XMODEM_COPY_BL      0x10003fe0u /* PacketReceived: bl CopyMemoryAligned */
#define USB_GUEST_XMODEM_WRITE_BL     0x10004018u /* PacketReceived: bl WriteBlockForImageTransfer */
#define USB_GUEST_PACKET_ASSERT_BHI1  0x10003ff0u /* bhi partsAlreadyInBuffer>4 */
#define USB_GUEST_PACKET_ASSERT_BHI2  0x10003ff4u /* bhi partsRemaining>8 */
#define USB_GUEST_PACKET_ASSERT_BLOCK 0x10003f8eu /* partsAlreadyInBuffer>4 cleanup */
#define USB_GUEST_DATA_BUFFER         0x2000caccu
#define USB_GUEST_DATA_BUFFER_END     (USB_GUEST_DATA_BUFFER + USB_GUEST_FLASH_BLOCK_BYTES)
#define USB_GUEST_PARAMETER_BUFFER    0x20016fc8u
#define USB_GUEST_PARAM_BUFFER_INDEX  0x20016fe8u
#define USB_GUEST_REGISTERS           0x20057038u
#define USB_GUEST_BLOCK_NUM           0x2000beb8u
#define USB_GUEST_PARTS_IN_BUFFER     0x20016fecu
#define USB_GUEST_VERIFICATION_ERRORS 0x200615d8u
#define USB_GUEST_PACKET_RECEIVED     0x10003f38u
#define USB_GUEST_XMODEM_RX_HI        0x10004054u /* xmodemrx + helpers */
#define USB_GUEST_XMODEM_CLEANUP      0x10003fa6u /* post-write: LED off, inc blockNum */
#define USB_GUEST_XMODEM_POST_WRITE   0x10003fb2u /* inc blockNum, reset buffer (no LED) */
#define USB_GUEST_TURN_ON_PICOLED     0x10004ed4u
#define USB_GUEST_TURN_OFF_PICOLED    0x10004ef0u
#define USB_GUEST_ALARM_POOL_RAM      0x200047a4u /* Pico SDK default alarm pool */
#define USB_GUEST_MUTEX_ENTER_V     0x10034e20u /* __recursive_mutex_enter_blocking_veneer */
#define USB_GUEST_MUTEX_EXIT_V      0x10034e10u /* __recursive_mutex_exit_veneer */
#define USB_GUEST_TS_READ_JEDECID   0x100030b8u /* tsReadJEDECID — stub JEDEC for emu flash */
#define USB_GUEST_INIT_FLASH          0x10003284u /* InitFlash */
#define USB_GUEST_SETUP_FLASH_MAP     0x10003430u /* SetupFlashUnitMapping */
#define USB_GUEST_GET_VOLUME_INFO     0x10004fa0u /* GetVolumeInfo */
#define USB_GUEST_GET_TOTAL_UNIT_COUNT 0x100034c0u /* GetTotalUnitCount */
#define USB_GUEST_GET_TOTAL_UNIT_COUNT_V 0x20004600u /* __GetTotalUnitCount_veneer (RAM) */
#define USB_GUEST_IS_VALID_UNIT_NUM   0x200011e4u /* IsValidUnitNum (RAM) */
#define USB_GUEST_IS_VALID_UNIT_NUM_V 0x10034db8u /* __IsValidUnitNum_veneer */
#define USB_GUEST_GET_BLOCK_COUNT     0x20001310u /* GetBlockCount (RAM) */
#define USB_GUEST_GET_BLOCK_COUNT_V   0x10034cc8u /* __GetBlockCount_veneer */
#define USB_GUEST_GET_UNIT_COUNT_FLASH_EN 0x20001194u /* GetUnitCountFlashEnabled (RAM) */
#define USB_GUEST_GET_UNIT_COUNT_FLASH_EN_V 0x10034e58u /* __GetUnitCountFlashEnabled_veneer */
#define USB_GUEST_GET_UNIT_COUNT_FLASH_ACT 0x100032f8u /* GetUnitCountFlashActual */
#define USB_GUEST_SET_FLASH_DRIVE_STR 0x10002d90u /* SetFlashDriveStrength */
#define USB_GUEST_ENABLE_4BYTE_ADDR   0x10002afcu /* Enable4BytesAddressing */
#define USB_GUEST_FLASH_MAP_ENABLED   0x20061616u
#define USB_GUEST_FLASH_UNIT_COUNT    0x200615b4u
#define USB_GUEST_FLASH_UNIT_MAP      0x200615b8u
#define USB_GUEST_READ_BLOCK_VENEER   0x10034e28u /* __ReadBlock_veneer */
#define USB_GUEST_READ_BLOCK          0x2000135cu /* ReadBlock (RAM) — DoReadBlock calls this */
#define USB_GUEST_WRITE_BLOCK_VENEER  0x10034dc8u /* __WriteBlock_veneer */
#define USB_GUEST_WRITE_BLOCK         0x200013f0u /* WriteBlock (RAM) */
#define USB_GUEST_CONFIG_BUFFER       0x2000bef4u
#define USB_GUEST_CONFIG_MAGIC        0x5e97724cu
#define USB_GUEST_CONFIG_FD_FLAGS_OFF 0xe2u
#define USB_GUEST_SETTINGS_NOT_FLASH  0x2006161fu
#define USB_GUEST_FLASH_SIZE0         0x2000d1ccu
#define USB_GUEST_FLASH_SIZE1         0x2000d1d0u
#define USB_GUEST_FLASH_CHIP0_UNITS   0x200615acu
#define USB_GUEST_FLASH_CHIP1_UNITS   0x200615b0u
#define USB_GUEST_FLASH_BLOCK_BYTES   512u
#define USB_GUEST_SPI_BAUDRATE_HZ     75000000u
#define USB_GUEST_WINBOND_JEDEC24     0xEF4020u
#define USB_GUEST_EMU_FLASH_CHIP_MB   64u
#define USB_GUEST_SPI_FLASH_CHIP_COUNT  2u
#define USB_GUEST_EXT_FLASH_UNIT_MB   32u
#define USB_GUEST_EXT_FLASH_BYTES_PER_UNIT \
#define USB_GUEST_EXT_FLASH_BLOCKS_PER_UNIT 65536u
#define USB_GUEST_SPI_FLASH_DEFAULT_DIR "flash"
#define USB_GUEST_SPI_FLASH1_DEFAULT    "flash/spi-flash1.bin"
#define USB_GUEST_SPI_FLASH2_DEFAULT    "flash/spi-flash2.bin"
#define USB_GUEST_EXIT              0x1000df80u /* _exit → BKPT loop */
#define USB_GUEST_PANIC             0x1000ae78u /* panic — skip BKPT _exit under emu */
#define USB_GUEST_HW_CLAIM_LOCK       0x1000aea8u /* hw_claim_lock */
#define USB_GUEST_HW_CLAIM_OR_ASSERT  0x1000aee0u /* hw_claim_or_assert */
#define USB_GUEST_HW_CLAIM_UNUSED     0x1000af12u /* hw_claim_unused_from_range */
#define USB_GUEST_HW_CLAIM_CLEAR_FAIL 0x1000af86u /* hw_claim_clear: bit not claimed */
#define USB_GUEST_HW_CLAIM_LOCK_BYTE  0x2000b79fu /* Pico SDK hw_claim mutex byte */
#define USB_GUEST_HW_CLAIM_BITMAP     0x200615e8u /* spin_lock / hw claim bits */
#define USB_GUEST_SPIN_LOCK_HW        0x2000b794u /* Pico SDK _sw_spin_locks */
#define USB_GUEST_U2_CRIT_SECTION     0x200600dcu /* u2_mon_cs */
#define USB_GUEST_U2_INIT             0x100067f0u /* U2_Init — skip Apple II U2 bus under emu */
#define USB_GUEST_U2_MONINIT_CALL     0x100067f8u /* U2_Init: bl U2_MonInit */
#define USB_GUEST_U2_RESET_CALL       0x10006812u /* U2_Init: bl u2_reset */
#define USB_GUEST_U2_MON_PUSH         0x10006bccu /* u2_mon_push — avoid ring/lock corruption */
#define USB_GUEST_U2_NET_INIT         0x10007ab0u /* U2_Net_Init */
#define USB_GUEST_U2_NET_POLL         0x10008150u /* U2_Net_Poll */
#define USB_GUEST_U2_MON_POLL_FLUSH   0x100070b4u /* U2_MonPollFlush */
#define USB_GUEST_WRAP_VPRINTF      0x1000e2c8u /* __wrap_vprintf */
#define USB_GUEST_VFPRINTF_R         0x1002f818u /* _vfprintf_r — skip locale/wchar body */
#define USB_GUEST_SVFPRINTF_R        0x1002d680u /* _svfprintf_r — sprintf path (hangs before _vfprintf_r) */
#define USB_GUEST_SVFIPRINTF_R       0x1002acd0u /* _svfiprintf_r — integer sprintf variant */
#define USB_GUEST_DO_GET_TIME_STR    0x10002150u /* DoGetTimeString (CMD_GETTIMESTR) */
#define USB_GUEST_DO_GET_TIME_STR_V  0x20004728u /* __DoGetTimeString_veneer */
#define USB_GUEST_LOCALE_MB_CUR_MAX 0x10032838u /* __locale_mb_cur_max — vfprintf locale spin */
#define USB_GUEST_VFPRINTF_LOOP_BACK  0x1002f818u /* unused on this build; VFPRINTF_R covers path */
#define USB_GUEST_VFPRINTF_LOOP_EXIT  0x10031aa4u
#define USB_GUEST_ASCII_MBTOWC      0x10034280u /* __ascii_mbtowc */
#define USB_GUEST_ASCII_MBTOWC_LOOP 0x1003429cu /* internal bne fallback */
#define USB_GUEST_VFPRINTF_LO       0x1002d680u /* cover _svfprintf_r .. _vfprintf_r */
#define USB_GUEST_VFPRINTF_HI       0x10031aa4u
#define USB_GUEST_USB_CONN_CMP      0x10000498u /* cmp r0,#0 after stdio_usb_connected */
#define USB_GUEST_USB_CONN_CMP_DBG  0x10000412u /* cbz after stdio_usb_connected (PicoW) */
#define USB_GUEST_NTPCLIENTFLAG 0x10u /* configbyte1 — matches MegaFlash defines.h */
#define USB_GUEST_WIFI_SSID           (USB_GUEST_CONFIG_BUFFER + 0x5Au)
#define USB_GUEST_TEST_WIFI           0x100085f4u /* TestWifi */
#define USB_GUEST_DO_TEST_WIFI        0x10001d1cu /* DoTestWifi (core1) */
#define USB_GUEST_GET_NETWORK_TIME    0x1000859cu /* GetNetworkTime */
#define USB_GUEST_CONNECT_WIFI        0x10008bacu /* CUDPTask::ConnectWifi */
#define USB_GUEST_INIT_CYW43          0x100088f4u /* CUDPTask::InitCyw43 */
#define USB_GUEST_CYW43_ARCH_INIT     0x1001b030u /* cyw43_arch_init (InitPicoLed) */
#define USB_GUEST_CYW43_GPIO_PUT      0x1001afa0u /* cyw43_arch_gpio_put */
#define USB_GUEST_NETERR_SSIDNOTSET   3u
#define USB_GUEST_NETERR_NONE         11u /* NetworkError_t: last enumerator */
#define USB_GUEST_DATA_XFER_MODE      0x2006160au /* dataBufferTransferMode */
#define USB_GUEST_DATA_BUFFER_INDEX   0x2000ccccu
#define USB_GUEST_MALLOC_MUTEX        0x20005164u /* malloc_mutex (.data, often still zero) */
#define USB_GUEST_MUTEX_ENTER_VENEER  0x10034da0u /* __mutex_enter_blocking_veneer */
#define USB_GUEST_MUTEX_EXIT_VENEER   0x10034d70u /* __mutex_exit_veneer */
#define USB_GUEST_SLEEP_UNTIL         0x1000bdc4u
#define USB_GUEST_SLEEP_US            0x1000be78u
#define USB_GUEST_SLEEP_MS            0x1000be9cu
#define USB_GUEST_TESTWIFI_ERRBYTE    USB_GUEST_PARAMETER_BUFFER
#define USB_GUEST_TESTWIFI_FLAG_A     USB_GUEST_DATA_XFER_MODE
#define USB_GUEST_TESTWIFI_FLAG_B     USB_GUEST_DATA_BUFFER_INDEX
#define USB_GUEST_TESTWIFI_PARAM      USB_GUEST_DATA_BUFFER
#define USB_GUEST_TESTWIFI_MISC       USB_GUEST_PARAM_BUFFER_INDEX

#endif
