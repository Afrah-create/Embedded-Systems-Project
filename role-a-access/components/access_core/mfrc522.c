#include "mfrc522.h"

#include <string.h>

#include "board_config.h"
#include "console_api.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"

/* SPI register addresses are already shifted, matching the MFRC522 datasheet
   section 8.1.2: bit 7 is the read flag and the address sits in bits 6..1. */
#define REG_COMMAND (0x01 << 1)
#define REG_COM_IRQ (0x04 << 1)
#define REG_DIV_IRQ (0x05 << 1)
#define REG_ERROR (0x06 << 1)
#define REG_FIFO_DATA (0x09 << 1)
#define REG_FIFO_LEVEL (0x0A << 1)
#define REG_CONTROL (0x0C << 1)
#define REG_BIT_FRAMING (0x0D << 1)
#define REG_COLL (0x0E << 1)
#define REG_MODE (0x11 << 1)
#define REG_TX_MODE (0x12 << 1)
#define REG_RX_MODE (0x13 << 1)
#define REG_TX_CONTROL (0x14 << 1)
#define REG_TX_ASK (0x15 << 1)
#define REG_MOD_WIDTH (0x24 << 1)
#define REG_T_MODE (0x2A << 1)
#define REG_T_PRESCALER (0x2B << 1)
#define REG_T_RELOAD_H (0x2C << 1)
#define REG_T_RELOAD_L (0x2D << 1)
#define REG_CRC_H (0x21 << 1)
#define REG_CRC_L (0x22 << 1)
#define REG_VERSION (0x37 << 1)

#define CMD_IDLE 0x00
#define CMD_CALC_CRC 0x03
#define CMD_TRANSCEIVE 0x0C
#define CMD_SOFT_RESET 0x0F

#define PICC_REQA 0x26
#define PICC_CT 0x88
#define PICC_SEL_CL1 0x93
#define PICC_SEL_CL2 0x95
#define PICC_SEL_CL3 0x97
#define PICC_HALT 0x50

typedef enum {
    ST_OK = 0,
    ST_ERROR,
    ST_TIMEOUT,
    ST_CRC,
    ST_NO_ROOM,
} mfrc_status_t;

typedef struct {
    spi_device_handle_t spi;
    gpio_num_t rst;
    bool ready;
    uint8_t version;
} mfrc522_dev_t;

static mfrc522_dev_t readers[2];
static SemaphoreHandle_t spi_mu;
static bool bus_ready;

static void short_delay(void)
{
    for (volatile int i = 0; i < 400; i++) {
    }
}

static esp_err_t xfer(mfrc522_dev_t *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t;

    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_transmit(dev->spi, &t);
}

static void write_reg(mfrc522_dev_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {reg, value};
    uint8_t rx[2];
    xfer(dev, tx, rx, 2);
}

static uint8_t read_reg(mfrc522_dev_t *dev, uint8_t reg)
{
    uint8_t tx[2] = {(uint8_t)(0x80 | reg), 0x00};
    uint8_t rx[2] = {0, 0};

    if (xfer(dev, tx, rx, 2) != ESP_OK) {
        return 0xFF;
    }
    return rx[1];
}

static void write_buf(mfrc522_dev_t *dev, uint8_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t tx[65];
    uint8_t rx[65];

    if (len > 64) {
        len = 64;
    }
    tx[0] = reg;
    memcpy(&tx[1], data, len);
    xfer(dev, tx, rx, (size_t)len + 1);
}

static void read_buf(mfrc522_dev_t *dev, uint8_t reg, uint8_t *values, uint8_t count, uint8_t rx_align)
{
    uint8_t tx[66];
    uint8_t rx[66];
    uint8_t addr = (uint8_t)(0x80 | reg);
    uint8_t index = 0;

    if (count == 0 || count > 64) {
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        tx[i] = addr;
    }
    tx[count] = 0;
    memset(rx, 0, sizeof(rx));
    if (xfer(dev, tx, rx, (size_t)count + 1) != ESP_OK) {
        return;
    }

    if (rx_align) {
        uint8_t mask = (uint8_t)(0xFF << rx_align);
        values[0] = (uint8_t)((values[0] & (uint8_t)~mask) | (rx[1] & mask));
        index = 1;
    }
    while (index < count) {
        values[index] = rx[index + 1];
        index++;
    }
}

static void set_mask(mfrc522_dev_t *dev, uint8_t reg, uint8_t mask)
{
    write_reg(dev, reg, (uint8_t)(read_reg(dev, reg) | mask));
}

static void clear_mask(mfrc522_dev_t *dev, uint8_t reg, uint8_t mask)
{
    write_reg(dev, reg, (uint8_t)(read_reg(dev, reg) & (uint8_t)~mask));
}

static mfrc_status_t crc_a(mfrc522_dev_t *dev, const uint8_t *data, uint8_t len, uint8_t *out)
{
    int64_t deadline;

    write_reg(dev, REG_COMMAND, CMD_IDLE);
    write_reg(dev, REG_DIV_IRQ, 0x04);
    write_reg(dev, REG_FIFO_LEVEL, 0x80);
    write_buf(dev, REG_FIFO_DATA, data, len);
    write_reg(dev, REG_COMMAND, CMD_CALC_CRC);

    deadline = esp_timer_get_time() + 90000;
    do {
        if (read_reg(dev, REG_DIV_IRQ) & 0x04) {
            write_reg(dev, REG_COMMAND, CMD_IDLE);
            out[0] = read_reg(dev, REG_CRC_L);
            out[1] = read_reg(dev, REG_CRC_H);
            return ST_OK;
        }
        taskYIELD();
    } while (esp_timer_get_time() < deadline);

    return ST_TIMEOUT;
}

static mfrc_status_t transceive(mfrc522_dev_t *dev, const uint8_t *send, uint8_t send_len,
                                uint8_t *back, uint8_t *back_len, uint8_t *valid_bits, uint8_t rx_align)
{
    uint8_t tx_last = valid_bits ? *valid_bits : 0;
    uint8_t framing = (uint8_t)((rx_align << 4) | tx_last);
    int64_t deadline;
    bool completed = false;
    uint8_t error;
    uint8_t n;

    write_reg(dev, REG_COMMAND, CMD_IDLE);
    write_reg(dev, REG_COM_IRQ, 0x7F);
    write_reg(dev, REG_FIFO_LEVEL, 0x80);
    write_buf(dev, REG_FIFO_DATA, send, send_len);
    write_reg(dev, REG_BIT_FRAMING, framing);
    write_reg(dev, REG_COMMAND, CMD_TRANSCEIVE);
    set_mask(dev, REG_BIT_FRAMING, 0x80);

    deadline = esp_timer_get_time() + 36000;
    do {
        n = read_reg(dev, REG_COM_IRQ);
        if (n & 0x30) {
            completed = true;
            break;
        }
        if (n & 0x01) {
            return ST_TIMEOUT;
        }
        taskYIELD();
    } while (esp_timer_get_time() < deadline);

    if (!completed) {
        return ST_TIMEOUT;
    }

    error = read_reg(dev, REG_ERROR);
    if (error & 0x13) {
        return ST_ERROR;
    }

    if (back && back_len) {
        n = read_reg(dev, REG_FIFO_LEVEL);
        if (n > *back_len) {
            return ST_NO_ROOM;
        }
        *back_len = n;
        read_buf(dev, REG_FIFO_DATA, back, n, rx_align);
        if (valid_bits) {
            *valid_bits = (uint8_t)(read_reg(dev, REG_CONTROL) & 0x07);
        }
    }

    return ST_OK;
}

static mfrc_status_t request_a(mfrc522_dev_t *dev)
{
    uint8_t cmd = PICC_REQA;
    uint8_t atqa[2] = {0, 0};
    uint8_t len = sizeof(atqa);
    uint8_t valid = 7;
    mfrc_status_t st;

    write_reg(dev, REG_TX_MODE, 0x00);
    write_reg(dev, REG_RX_MODE, 0x00);
    write_reg(dev, REG_MOD_WIDTH, 0x26);
    clear_mask(dev, REG_COLL, 0x80);

    st = transceive(dev, &cmd, 1, atqa, &len, &valid, 0);
    if (st != ST_OK) {
        return st;
    }
    if (len != 2 || valid != 0) {
        return ST_ERROR;
    }
    return ST_OK;
}

static mfrc_status_t select_uid(mfrc522_dev_t *dev, uint8_t *uid, uint8_t *uid_len)
{
    uint8_t uid_out[7];
    uint8_t uid_bytes = 0;
    uint8_t level = 1;

    clear_mask(dev, REG_COLL, 0x80);

    while (level <= 3) {
        uint8_t sel = level == 1 ? PICC_SEL_CL1 : (level == 2 ? PICC_SEL_CL2 : PICC_SEL_CL3);
        uint8_t cmd[2] = {sel, 0x20};
        uint8_t resp[7] = {0};
        uint8_t resp_len = sizeof(resp);
        uint8_t valid = 0;
        uint8_t frame[9];
        uint8_t sak[3] = {0};
        uint8_t sak_len = sizeof(sak);
        uint8_t crc[2];
        uint8_t start;
        uint8_t ncopy;
        mfrc_status_t st = transceive(dev, cmd, 2, resp, &resp_len, &valid, 0);

        if (st != ST_OK || resp_len < 5) {
            return st == ST_OK ? ST_ERROR : st;
        }
        if ((uint8_t)(resp[0] ^ resp[1] ^ resp[2] ^ resp[3]) != resp[4]) {
            return ST_ERROR;
        }

        frame[0] = sel;
        frame[1] = 0x70;
        memcpy(&frame[2], resp, 4);
        frame[6] = resp[4];
        st = crc_a(dev, frame, 7, &frame[7]);
        if (st != ST_OK) {
            return st;
        }

        valid = 0;
        st = transceive(dev, frame, 9, sak, &sak_len, &valid, 0);
        if (st != ST_OK || sak_len != 3 || valid != 0) {
            return st == ST_OK ? ST_ERROR : st;
        }
        st = crc_a(dev, sak, 1, crc);
        if (st != ST_OK) {
            return st;
        }
        if (crc[0] != sak[1] || crc[1] != sak[2]) {
            return ST_CRC;
        }

        start = resp[0] == PICC_CT ? 1 : 0;
        ncopy = resp[0] == PICC_CT ? 3 : 4;
        if ((uint8_t)(uid_bytes + ncopy) > sizeof(uid_out)) {
            return ST_ERROR;
        }
        memcpy(&uid_out[uid_bytes], &resp[start], ncopy);
        uid_bytes = (uint8_t)(uid_bytes + ncopy);

        if (sak[0] & 0x04) {
            level++;
            continue;
        }

        memcpy(uid, uid_out, uid_bytes);
        *uid_len = uid_bytes;
        return ST_OK;
    }

    return ST_ERROR;
}

static void halt_a(mfrc522_dev_t *dev)
{
    uint8_t frame[4] = {PICC_HALT, 0, 0, 0};
    uint8_t dummy[2];
    uint8_t dummy_len = sizeof(dummy);
    uint8_t valid = 0;

    if (crc_a(dev, frame, 2, &frame[2]) != ST_OK) {
        return;
    }
    /* ISO 14443 treats a timeout here as success: the card has gone silent. */
    (void)transceive(dev, frame, sizeof(frame), dummy, &dummy_len, &valid, 0);
}

static void hard_reset(mfrc522_dev_t *dev)
{
    gpio_set_level(dev->rst, 0);
    short_delay();
    gpio_set_level(dev->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void chip_setup(mfrc522_dev_t *dev)
{
    uint8_t tx;

    write_reg(dev, REG_TX_MODE, 0x00);
    write_reg(dev, REG_RX_MODE, 0x00);
    write_reg(dev, REG_MOD_WIDTH, 0x26);
    write_reg(dev, REG_T_MODE, 0x80);
    write_reg(dev, REG_T_PRESCALER, 0xA9);
    write_reg(dev, REG_T_RELOAD_H, 0x03);
    write_reg(dev, REG_T_RELOAD_L, 0xE8);
    write_reg(dev, REG_TX_ASK, 0x40);
    write_reg(dev, REG_MODE, 0x3D);

    tx = read_reg(dev, REG_TX_CONTROL);
    if ((tx & 0x03) != 0x03) {
        write_reg(dev, REG_TX_CONTROL, (uint8_t)(tx | 0x03));
    }
    dev->version = read_reg(dev, REG_VERSION);
}

static esp_err_t add_reader(int index, gpio_num_t cs, gpio_num_t rst, const char *name)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4000000,
        .mode = 0,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << rst,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err;

    readers[index].rst = rst;
    readers[index].ready = false;

    err = gpio_config(&rst_cfg);
    if (err != ESP_OK) {
        console_printf("RFID %s reset pin failed\n", name);
        return err;
    }
    gpio_set_level(rst, 1);

    err = spi_bus_add_device(SPI3_HOST, &devcfg, &readers[index].spi);
    if (err != ESP_OK) {
        console_printf("RFID %s SPI device failed: %s\n", name, esp_err_to_name(err));
        return err;
    }

    hard_reset(&readers[index]);
    chip_setup(&readers[index]);
    readers[index].ready = true;
    console_printf("RFID %s version=0x%02X cs=%d rst=%d\n", name, readers[index].version, (int)cs, (int)rst);
    return ESP_OK;
}

esp_err_t mfrc522_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SPI_MOSI,
        .miso_io_num = PIN_SPI_MISO,
        .sclk_io_num = PIN_SPI_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 128,
    };
    esp_err_t err;

    if (spi_mu == NULL) {
        spi_mu = xSemaphoreCreateMutex();
    }
    if (spi_mu == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!bus_ready) {
        err = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            console_printf("SPI bus init failed: %s\n", esp_err_to_name(err));
            return err;
        }
        bus_ready = true;
    }

    err = add_reader(RFID_ENTRY, PIN_RFID_IN_CS, PIN_RFID_IN_RST, "entry");
    if (err != ESP_OK) {
        return err;
    }
    return add_reader(RFID_EXIT, PIN_RFID_OUT_CS, PIN_RFID_OUT_RST, "exit");
}

bool mfrc522_ready(int index)
{
    if (index < 0 || index > 1) {
        return false;
    }
    return readers[index].ready;
}

esp_err_t mfrc522_reinit(int index)
{
    if (index < 0 || index > 1 || readers[index].spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(spi_mu, portMAX_DELAY);
    hard_reset(&readers[index]);
    chip_setup(&readers[index]);
    readers[index].ready = true;
    xSemaphoreGive(spi_mu);
    return ESP_OK;
}

bool mfrc522_read_uid(int index, uint8_t *uid, uint8_t *uid_len)
{
    mfrc_status_t st;
    bool ok = false;

    if (!mfrc522_ready(index) || uid == NULL || uid_len == NULL) {
        return false;
    }

    /* Both readers share one SPI bus. The mutex covers a whole card
       exchange so the two reader tasks cannot interleave bytes. */
    xSemaphoreTake(spi_mu, portMAX_DELAY);
    st = request_a(&readers[index]);
    if (st == ST_OK) {
        st = select_uid(&readers[index], uid, uid_len);
        if (st == ST_OK && (*uid_len == 4 || *uid_len == 7)) {
            halt_a(&readers[index]);
            ok = true;
        }
    }
    xSemaphoreGive(spi_mu);
    return ok;
}
