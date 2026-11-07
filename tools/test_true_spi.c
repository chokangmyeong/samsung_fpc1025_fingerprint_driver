#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define VID 0x04e8
#define PID 0x7301
#define EP_OUT 0x01
#define EP_IN  0x82

static libusb_context *ctx;
static libusb_device_handle *dev;

static int bridge_gpio(uint8_t val)
{
    return libusb_control_transfer(dev, 0x40, 0xDB, 6, val, NULL, 0, 1000);
}

static int bridge_read_irq(void)
{
    uint8_t data[2] = {0};
    int r = libusb_control_transfer(dev, 0xC0, 0xDA, 7, 0, data, 2, 1000);
    if (r == 2 && data[0] == 0) return data[1];
    return -1;
}

static void LIBUSB_CALL out_cb(struct libusb_transfer *t)
{
    int *done_flag = (int *)t->user_data;
    *done_flag = 1;
}

static int spi_xfer(const uint8_t *tx, uint8_t *rx, int len)
{
    // 1. 0x180008458: Configure SPI bus (3 MHz, 8-bit)
    uint8_t spi_cfg[16] = {
        0xC0, 0xC6, 0x2D, 0x00,
        0x08, 0x00, 0xFF, 0x01,
        0x01, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    int r = libusb_control_transfer(dev, 0x40, 0xC3, 0, 0, spi_cfg, 16, 1000);
    if (r < 0) {
        fprintf(stderr, "0xC3 failed: %d\n", r);
        return -1;
    }

    // 2. 0x1800086fc: Set SPI transaction byte length (wValue=3, wIndex=len)
    r = libusb_control_transfer(dev, 0x40, 0xCA, 3, (uint16_t)len, NULL, 0, 1000);
    if (r < 0) {
        fprintf(stderr, "0xCA failed: %d\n", r);
        return -1;
    }

    // 3. For len <= 512, synchronous Bulk OUT + Bulk IN fits in the 576-byte bridge FIFO.
    //    For len > 512 (25602-byte full frame), submit Bulk OUT async while reading Bulk IN.
    int out_done = 1;
    struct libusb_transfer *t_out = NULL;
    if (len <= 512) {
        int done = 0;
        r = libusb_bulk_transfer(dev, EP_OUT, (uint8_t *)tx, len, &done, 2000);
        if (r != 0 || done != len) {
            fprintf(stderr, "Sync Bulk OUT failed (%d): r=%d done=%d\n", len, r, done);
            return -1;
        }
    } else {
        out_done = 0;
        t_out = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(t_out, dev, EP_OUT, (uint8_t *)tx, len, out_cb, &out_done, 4000);
        r = libusb_submit_transfer(t_out);
        if (r < 0) {
            fprintf(stderr, "Async Bulk OUT submit failed: %d\n", r);
            libusb_free_transfer(t_out);
            return -1;
        }
    }

    int got = 0;
    uint8_t *rx_buf = rx ? rx : malloc(len + 512);
    while (got < len) {
        int c = 0;
        r = libusb_bulk_transfer(dev, EP_IN, rx_buf + got, len - got, &c, 4000);
        if (r != 0) {
            fprintf(stderr, "Bulk IN failed at %d/%d: r=%d\n", got, len, r);
            break;
        }
        if (c == 0) break;
        got += c;
    }

    while (!out_done) {
        libusb_handle_events_completed(ctx, &out_done);
    }
    if (t_out) libusb_free_transfer(t_out);
    if (!rx) free(rx_buf);
    return (got == len) ? 0 : -1;
}

static int fpc_write(const uint8_t *pkt, int len)
{
    return spi_xfer(pkt, NULL, len);
}

static int fpc_read(uint8_t opcode, int dummy_bytes, uint8_t *out, int out_len)
{
    int total = 1 + dummy_bytes + out_len;
    uint8_t *tx = calloc(1, total);
    uint8_t *rx = calloc(1, total);
    tx[0] = opcode;
    int r = spi_xfer(tx, rx, total);
    if (r == 0) {
        memcpy(out, rx + 1 + dummy_bytes, out_len);
    }
    free(tx);
    free(rx);
    return r;
}

static int fpc_cmd(uint8_t opcode, uint8_t *irq_out)
{
    if (spi_xfer(&opcode, NULL, 1) != 0) return -1;
    for (int i = 0; i < 30; i++) {
        int irq = bridge_read_irq();
        if (irq > 0) {
            uint8_t st = 0;
            fpc_read(0x1C, 0, &st, 1);
            if (irq_out) *irq_out = st;
            return 0;
        }
        usleep(1000);
    }
    uint8_t st = 0;
    fpc_read(0x1C, 0, &st, 1);
    if (irq_out) *irq_out = st;
    return 0;
}

static int fpc_capture_bytes(uint8_t *buf, int len)
{
    uint8_t irq = 0;
    if (fpc_cmd(0xC0, &irq) != 0) return -1;
    return fpc_read(0xC4, 1, buf, len);
}

static int avg_bytes(const uint8_t *buf, int len)
{
    int sum = 0;
    for (int i = 0; i < len; i++) sum += buf[i];
    return sum / len;
}

static uint8_t shift_table[32];
static uint16_t gain_table[16];
static uint8_t cur_pxl_idx = 2;

static int set_pxlctrl(uint8_t idx)
{
    static const uint8_t tbl[4] = {0x1E, 0x0E, 0x1A, 0x0A};
    uint8_t pkt[3] = {0xA8, 0x0F, tbl[idx & 3]};
    return fpc_write(pkt, 3);
}

static void calibrate_adc(void)
{
    uint8_t sub[448];
    const uint8_t r_78[] = {0x78, 0xFF, 0xFF};
    const uint8_t r_8c_test[] = {0x8C, 0x04};
    const uint8_t r_54_sub[] = {0x54, 0x20, 0x0E, 0x08, 0x20};
    fpc_write(r_78, sizeof(r_78));
    fpc_write(r_8c_test, sizeof(r_8c_test));
    set_pxlctrl(0);
    fpc_write(r_54_sub, sizeof(r_54_sub));

    int prev_avg = 0;
    uint8_t sil = 0;
    for (int s = 0; s < 32; s++) {
        uint8_t r_a0[3] = {0xA0, (uint8_t)s, 0x00};
        fpc_write(r_a0, 3);
        fpc_capture_bytes(sub, 448);
        int avg = avg_bytes(sub, 448);
        if (prev_avg < 200 && avg >= 200) sil = s;
        shift_table[s] = (uint8_t)(avg - prev_avg);
        prev_avg = avg;
    }

    gain_table[0] = 1000;
    int need_base = 1;
    int base_avg = 0;
    for (int g = 0; g < 15; g++) {
        if (need_base) {
            uint8_t r_a0[3] = {0xA0, sil, (uint8_t)g};
            fpc_write(r_a0, 3);
            fpc_capture_bytes(sub, 448);
            base_avg = avg_bytes(sub, 448);
            need_base = 0;
        }
        uint8_t r_a0_next[3] = {0xA0, sil, (uint8_t)(g + 1)};
        fpc_write(r_a0_next, 3);
        fpc_capture_bytes(sub, 448);
        int next_avg = avg_bytes(sub, 448);
        int c0 = base_avg < 128 ? 128 : base_avg;
        int c1 = next_avg < 129 ? 129 : next_avg;
        gain_table[g + 1] = (uint16_t)(((c1 - 127) * 1000) / (c0 - 127));
        base_avg = c1;
        while (base_avg > 200 && sil > 0) {
            sil--;
            need_base = 1;
            base_avg -= shift_table[sil];
        }
    }

    const uint8_t r_54_full[] = {0x54, 0x00, 0xA0, 0x00, 0xA0};
    const uint8_t r_8c_norm[] = {0x8C, 0x12};
    fpc_write(r_54_full, sizeof(r_54_full));
    fpc_write(r_8c_norm, sizeof(r_8c_norm));
    set_pxlctrl(2);

    printf("[+] Calibrated shift_table: ");
    for (int i = 0; i < 32; i++) printf("%d ", shift_table[i]);
    printf("\n[+] Calibrated gain_table:  ");
    for (int i = 0; i < 16; i++) printf("%d ", gain_table[i]);
    printf("\n");
}

static void hist_bounds(const uint8_t *buf, int len, int *p_low, int *p_high)
{
    uint16_t hist[256] = {0};
    for (int i = 0; i < len; i++) hist[buf[i]]++;
    int sum_lo = 0, sum_hi = 0;
    *p_low = -1;
    *p_high = -1;
    for (int i = 0; i < 256; i++) {
        sum_lo += hist[i];
        sum_hi += hist[255 - i];
        if (sum_lo > 1 && *p_low < 0) *p_low = i;
        if (sum_hi > 1 && *p_high < 0) *p_high = 255 - i;
    }
    if (*p_low < 0) *p_low = 0;
    if (*p_high < 0) *p_high = 255;
}

static void auto_gain_control(void)
{
    uint8_t sub[448];
    const uint8_t r_54_sub[] = {0x54, 73, 14, 64, 32};
    const uint8_t r_a0_init[] = {0xA0, 0x10, 0x00};
    const uint8_t r_64_sub[] = {0x64, 0x0C};
    const uint8_t r_5c_sub[] = {0x5C, 0x0A};

    fpc_write(r_54_sub, sizeof(r_54_sub));
    fpc_write(r_a0_init, sizeof(r_a0_init));
    set_pxlctrl(cur_pxl_idx);
    fpc_write(r_64_sub, sizeof(r_64_sub));
    fpc_write(r_5c_sub, sizeof(r_5c_sub));

    fpc_capture_bytes(sub, 448);
    int zeros = 0, ones = 0;
    for (int i = 0; i < 448; i++) {
        if (sub[i] == 0x00) zeros++;
        if (sub[i] == 0xFF) ones++;
    }
    int p_lo = 0, p_hi = 255;
    hist_bounds(sub, 448, &p_lo, &p_hi);
    int mid1 = (p_hi + p_lo) / 2;

    uint8_t next_idx = cur_pxl_idx;
    if (next_idx < 3 && (mid1 > 0xA2 || ones > 10)) {
        next_idx++;
    } else if (next_idx > 0 && (mid1 < 0x76 || zeros > 10)) {
        next_idx--;
    }
    if (next_idx != cur_pxl_idx) {
        cur_pxl_idx = next_idx;
        set_pxlctrl(cur_pxl_idx);
        fpc_capture_bytes(sub, 448);
        hist_bounds(sub, 448, &p_lo, &p_hi);
        mid1 = (p_hi + p_lo) / 2;
    }

    uint8_t irq = 0, d4[4] = {0};
    fpc_cmd(0x20, &irq);
    fpc_read(0xD4, 0, d4, 4);

    fpc_capture_bytes(sub, 448);
    int p_lo2 = 0, p_hi2 = 255;
    hist_bounds(sub, 448, &p_lo2, &p_hi2);
    int mid2 = (p_hi2 + p_lo2) / 2;

    int diff = mid1 - mid2;
    int target = 0xA2;
    if (diff < 0) target = 0x7D;
    else if (diff > 13) target = 0xA2;
    else if (diff > 8) target = 0x9B;
    else if (diff > 6) target = 0x96;
    else if (diff > 3) target = 0x91;
    else target = 0x82;

    uint8_t shift = 0x10;
    int val = mid2;
    if (val < target) {
        while (val < target && shift < 0x20) {
            shift++;
            val += shift_table[shift < 32 ? shift : 31];
        }
    } else if (val > target) {
        while (val > target && shift > 0) {
            shift--;
            val -= shift_table[shift];
        }
    }

    int span = p_hi2 - p_lo2;
    uint8_t gain = 0;
    while (gain < 0x0F) {
        span = (span * gain_table[gain]) / 1000;
        if (span > 0x84) break;
        gain++;
    }

    printf("[+] AGC result: pxl_idx=%d, mid1=%d, mid2=%d, span_in=%d -> shift=0x%02X, gain=0x%02X (span_out=%d)\n",
           cur_pxl_idx, mid1, mid2, p_hi2 - p_lo2, shift, gain, span);

    const uint8_t r_a0_final[] = {0xA0, shift, gain};
    const uint8_t r_64_full[] = {0x64, 0x1E};
    const uint8_t r_5c_full[] = {0x5C, 0x0B};
    const uint8_t r_54_full[] = {0x54, 0x00, 0xA0, 0x00, 0xA0};
    fpc_write(r_a0_final, sizeof(r_a0_final));
    fpc_write(r_64_full, sizeof(r_64_full));
    fpc_write(r_5c_full, sizeof(r_5c_full));
    fpc_write(r_54_full, sizeof(r_54_full));
}

int main(void)
{
    if (libusb_init(&ctx) != 0) return 1;
    dev = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!dev) { fprintf(stderr, "Cannot open device\n"); return 1; }
    libusb_set_auto_detach_kernel_driver(dev, 1);
    if (libusb_claim_interface(dev, 0) != 0) return 1;

    // Reset sequence matching 0x1800041fe
    bridge_gpio(0);
    usleep(5000);
    bridge_gpio(1);
    usleep(20000);

    uint8_t irq = 0;
    fpc_cmd(0xF8, &irq);
    printf("[+] Soft reset (0xF8) -> IRQ status = 0x%02X\n", irq);

    uint8_t hwid[2] = {0};
    fpc_read(0xFC, 0, hwid, 2);
    printf("[+] HWID (0xFC) = 0x%02X 0x%02X\n", hwid[0], hwid[1]);

    // Init sequence from 0x180005ee4 (swing = 1 branch)
    const uint8_t r_9c[] = {0x9C, 0x55, 0x40, 0x00, 0x2D, 0x24};
    const uint8_t r_6c[] = {0x6C, 0x29};
    const uint8_t r_90[] = {0x90, 0x32};
    const uint8_t r_68[] = {0x68, 0x31, 0x31, 0x31, 0x31, 0x3D, 0x3D, 0x3D, 0x3D};
    const uint8_t r_98[] = {0x98, 0x03, 0xEB, 0x3F, 0xB8, 0x0A, 0x07, 0x11, 0x22, 0x6A, 0x5D};
    const uint8_t r_8c[] = {0x8C, 0x12};
    const uint8_t r_5c[] = {0x5C, 0x0B};
    const uint8_t r_d8[] = {0xD8, 0x20};
    const uint8_t r_dc[] = {0xDC, 0x00, 0x0A};

    fpc_write(r_9c, sizeof(r_9c));
    fpc_write(r_6c, sizeof(r_6c));
    fpc_write(r_90, sizeof(r_90));
    fpc_write(r_68, sizeof(r_68));
    fpc_write(r_98, sizeof(r_98));
    fpc_write(r_8c, sizeof(r_8c));
    fpc_write(r_5c, sizeof(r_5c));
    fpc_write(r_d8, sizeof(r_d8));
    fpc_write(r_dc, sizeof(r_dc));

    // Calibrate ADC shift & gain tables (0x180003694)
    calibrate_adc();

    // Set AFD pxlctrl & adcshiftgain from 0x180004074 / 0x180004641 / 0x18002f058
    const uint8_t afd_a8[] = {0xA8, 0x0F, 0x0A};
    const uint8_t afd_a0[] = {0xA0, 0x0A, 0x02};
    fpc_write(afd_a8, sizeof(afd_a8));
    fpc_write(afd_a0, sizeof(afd_a0));

    // Test 0x180007324: FNGR_PRESENT_QUERY (0x20) + read 0xD4 (4 bytes)
    for (int t = 0; t < 3; t++) {
        fpc_cmd(0x20, &irq);
        uint8_t d4[4] = {0};
        fpc_read(0xD4, 0, d4, 4);
        uint16_t mask = ((uint16_t)d4[0] << 8) | d4[1];
        printf("[+] Query %d: 0x20 -> irq=0x%02X, 0xD4=%02X %02X %02X %02X (mask=0x%03X, pop=%d)\n",
               t, irq, d4[0], d4[1], d4[2], d4[3], mask & 0x0FFF, __builtin_popcount(mask & 0x0FFF));
    }

    // Run Auto Gain Control (0x180003a30)
    auto_gain_control();

    // Capture image: 0xC0 + wait IRQ + 0xC4 (25602 bytes in ONE SPI transaction!)
    fpc_cmd(0xC0, &irq);
    printf("[+] Capture (0xC0) -> IRQ status = 0x%02X\n", irq);

    uint8_t *frame = calloc(1, 25600);
    int r = fpc_read(0xC4, 1, frame, 25600);
    printf("[+] Read 25600 bytes via single 25602-byte SPI xfer: r=%d\n", r);
    printf("    First 32 bytes: ");
    for (int i = 0; i < 32; i++) printf("%02X ", frame[i]);
    printf("\n    Bytes 64..95:   ");
    for (int i = 64; i < 96; i++) printf("%02X ", frame[i]);
    printf("\n    Bytes 25568..25599: ");
    for (int i = 25568; i < 25600; i++) printf("%02X ", frame[i]);
    printf("\n");

    libusb_release_interface(dev, 0);
    libusb_close(dev);
    libusb_exit(ctx);
    return 0;
}
