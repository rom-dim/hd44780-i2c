#include <linux/cdev.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/kernel.h>

#include "hd44780.h"

#define BL	0x08
#define E	0x04
#define RW	0x02
#define RS	0x01

#define HD44780_CLEAR_DISPLAY	0x01
#define HD44780_RETURN_HOME	0x02
#define HD44780_ENTRY_MODE_SET	0x04
#define HD44780_DISPLAY_CTRL	0x08
#define HD44780_SHIFT		0x10
#define HD44780_FUNCTION_SET	0x20
#define HD44780_CGRAM_ADDR	0x40
#define HD44780_DDRAM_ADDR	0x80

#define HD44780_DL_8BITS	0x10
#define HD44780_DL_4BITS	0x00
#define HD44780_N_2LINES	0x08
#define HD44780_N_1LINE		0x00

#define HD44780_D_DISPLAY_ON	0x04
#define HD44780_D_DISPLAY_OFF	0x00
#define HD44780_C_CURSOR_ON	0x02
#define HD44780_C_CURSOR_OFF	0x00
#define HD44780_B_BLINK_ON	0x01
#define HD44780_B_BLINK_OFF	0x00

#define HD44780_ID_INCREMENT	0x02
#define HD44780_ID_DECREMENT	0x00
#define HD44780_S_SHIFT_ON	0x01
#define HD44780_S_SHIFT_OFF	0x00

static struct hd44780_geometry hd44780_geometry_20x4 = {
    .cols = 20,
    .rows = 4,
    .start_addrs = {0x00, 0x40, 0x14, 0x54},
};

static struct hd44780_geometry hd44780_geometry_20x2 = {
    .cols = 20,
    .rows = 2,
    .start_addrs = {0x00, 0x40, 0x00, 0x54},
};

static struct hd44780_geometry hd44780_geometry_16x2 = {
    .cols = 16,
    .rows = 2,
    .start_addrs = {0x00, 0x40},
};

static struct hd44780_geometry hd44780_geometry_8x1 = {
    .cols = 8,
    .rows = 1,
    .start_addrs = {0x00},
};

struct hd44780_geometry *hd44780_geometries[] = {
    &hd44780_geometry_20x4,
    &hd44780_geometry_20x2,
    &hd44780_geometry_16x2,
    &hd44780_geometry_8x1,
    NULL
};

/* Defines possible register that we can write to */
typedef enum { IR, DR } dest_reg;

static void pcf8574_raw_write(struct hd44780 *lcd, u8 data)
{
    i2c_smbus_write_byte(lcd->i2c_client, data);
}

static void hd44780_write_nibble(struct hd44780 *lcd, dest_reg reg, u8 data)
{
    /* Shift the interesting data on the upper 4 bits (b7-b4) */
    data = (data << 4) & 0xF0;

    /* Flip the RS bit if we write do data register */
    if (reg == DR)
        data |= RS;

    /* Keep the RW bit low, because we write */
    data = data | (RW & 0x00);

    /* Flip the backlight bit */
    if (lcd->backlight)
        data |= BL;

    pcf8574_raw_write(lcd, data);
    /* Theoretically wait for tAS = 40ns, practically it's already elapsed */

    /* Raise the E signal... */
    pcf8574_raw_write(lcd, data | E);
    /* Again, "wait" for pwEH = 230ns */

    /* ...and let it fall to clock the data into the HD44780's register */
    pcf8574_raw_write(lcd, data);
    /* And again, "wait" for about tCYC_E - pwEH = 270ns */
}

/*
 * Takes a regular 8-bit instruction and writes it's high nibble into device's
 * instruction register. The low nibble is assumed to be all zeros. This is
 * used with a physical 4-bit bus when the device is still expecting 8-bit
 * instructions.
 */
static void hd44780_write_instruction_high_nibble(struct hd44780 *lcd, u8 data)
{
    u8 h = (data >> 4) & 0x0F;

    hd44780_write_nibble(lcd, IR, h);

    udelay(37);
}

static void hd44780_write_instruction(struct hd44780 *lcd, u8 data)
{
    u8 h = (data >> 4) & 0x0F;
    u8 l = data & 0x0F;

    hd44780_write_nibble(lcd, IR, h);
    hd44780_write_nibble(lcd, IR, l);

    udelay(37);
}

static void hd44780_write_data(struct hd44780 *lcd, u8 data)
{
    u8 h = (data >> 4) & 0x0F;
    u8 l = data & 0x0F;

    hd44780_write_nibble(lcd, DR, h);
    hd44780_write_nibble(lcd, DR, l);

    udelay(37 + 4);
}

static void hd44780_write_char(struct hd44780 *lcd, char ch)
{
    struct hd44780_geometry *geo = lcd->geometry;

    hd44780_write_data(lcd, ch);

    lcd->pos.col++;

    if (lcd->pos.col == geo->cols) {
        lcd->pos.row = (lcd->pos.row + 1) % geo->rows;
        lcd->pos.col = 0;
        hd44780_write_instruction(lcd, HD44780_DDRAM_ADDR | geo->start_addrs[lcd->pos.row]);
    }
}

static void hd44780_clear_display(struct hd44780 *lcd)
{
    hd44780_write_instruction(lcd, HD44780_CLEAR_DISPLAY);

    /* Wait for 1.64 ms because this one needs more time */
    udelay(1640);

    /*
     * CLEAR_DISPLAY instruction also returns cursor to home,
     * so we need to update it locally.
     */
    lcd->pos.row = 0;
    lcd->pos.col = 0;
}

static void hd44780_clear_line(struct hd44780 *lcd)
{
    struct hd44780_geometry *geo;
    int start_addr, col;

    geo = lcd->geometry;
    start_addr = geo->start_addrs[lcd->pos.row];

    hd44780_write_instruction(lcd, HD44780_DDRAM_ADDR | start_addr);

    for (col = 0; col < geo->cols; col++)
        hd44780_write_data(lcd, ' ');

    hd44780_write_instruction(lcd, HD44780_DDRAM_ADDR | start_addr);
}

static void hd44780_handle_setcursor(struct hd44780 *lcd, unsigned char row, unsigned char col)
{
    struct hd44780_geometry *geo = lcd->geometry;

    lcd->pos.col = col;
    lcd->pos.row = row;

    if (lcd->pos.col >= geo->cols) {
        lcd->pos.row = (lcd->pos.row + 1) % geo->rows;
        lcd->pos.col = 0;
    }

    if (lcd->pos.row >= geo->rows) {
        lcd->pos.row = 0;
    }

    hd44780_write_instruction(lcd, HD44780_DDRAM_ADDR | (geo->start_addrs[lcd->pos.row] + lcd->pos.col));
}

static void hd44780_handle_tab(struct hd44780 *lcd)
{
    struct hd44780_geometry *geo = lcd->geometry;

    lcd->pos.col += 4;

    if (lcd->pos.col >= geo->cols) {
        lcd->pos.row = (lcd->pos.row + 1) % geo->rows;
        lcd->pos.col = 0;
    }

    hd44780_write_instruction(lcd, HD44780_DDRAM_ADDR | (geo->start_addrs[lcd->pos.row] + lcd->pos.col));
}

static void hd44780_handle_new_line(struct hd44780 *lcd)
{
    struct hd44780_geometry *geo = lcd->geometry;

    lcd->pos.row = (lcd->pos.row + 1) % geo->rows;
    lcd->pos.col = 0;
    hd44780_write_instruction(lcd, HD44780_DDRAM_ADDR
                              | geo->start_addrs[lcd->pos.row]);
    hd44780_clear_line(lcd);
}

static void hd44780_handle_carriage_return(struct hd44780 *lcd)
{
    struct hd44780_geometry *geo = lcd->geometry;

    lcd->pos.col = 0;
    hd44780_write_instruction(lcd, HD44780_DDRAM_ADDR
                              | geo->start_addrs[lcd->pos.row]);
}

static void hd44780_leave_esc_seq(struct hd44780 *lcd)
{
    memset(lcd->esc_seq_buf.buf, 0, ESC_SEQ_BUF_SIZE);
    memset(lcd->esc_seq_buf.param, 0, NUM_ESC_PARAMS*sizeof(long));
    lcd->esc_seq_buf.param_index = 0;
    lcd->esc_seq_buf.length = 0;
    lcd->esc_seq_buf.param_err = 0;
    lcd->is_in_esc_seq = false;
}

static void hd44780_flush_esc_seq(struct hd44780 *lcd)
{
    char *buf_to_flush;
    int buf_length;

    /* Copy and reset current esc seq */
    buf_to_flush = kmalloc(sizeof(char) * ESC_SEQ_BUF_SIZE, GFP_KERNEL);
    memcpy(buf_to_flush, lcd->esc_seq_buf.buf, ESC_SEQ_BUF_SIZE);
    buf_length = lcd->esc_seq_buf.length;

    hd44780_leave_esc_seq(lcd);

    /* Write \e that initiated current esc seq */
    hd44780_write_char(lcd, '\e');

    /* Flush current esc seq */
    hd44780_write(lcd, buf_to_flush, buf_length);

    kfree(buf_to_flush);
}

void hd44780_flush(struct hd44780 *lcd)
{
    while (lcd->is_in_esc_seq)
        hd44780_flush_esc_seq(lcd);
}

void hd44780_add_new_vt100_param(struct hd44780 *lcd)
{
    if(lcd->esc_seq_buf.length == 0)
        return;

    if(lcd->esc_seq_buf.param_index >= NUM_ESC_PARAMS)
    {
        lcd->esc_seq_buf.param_err = 1;
        return;
    }

    if (kstrtol(lcd->esc_seq_buf.buf, 0, &lcd->esc_seq_buf.param[lcd->esc_seq_buf.param_index]) != 0)
    {
        lcd->esc_seq_buf.param_err = 1;
        return;
    }

    lcd->esc_seq_buf.param_index++;
    lcd->esc_seq_buf.length = 0;
    memset(lcd->esc_seq_buf.buf, 0, ESC_SEQ_BUF_SIZE);

}

static void hd44780_handle_esc_seq_char(struct hd44780 *lcd, char ch)
{
    static enum {CMD_START,CMD_PARAM,CMD_END}VT100_state = CMD_START;

    switch(VT100_state)
    {
    case CMD_START:
        if(ch == '[')
            VT100_state = CMD_PARAM;
        break;

    case CMD_PARAM:
        switch(ch)
        {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            lcd->esc_seq_buf.buf[lcd->esc_seq_buf.length++] = ch;
            return;

        case ';':
            hd44780_add_new_vt100_param(lcd);
            return;

        default:
            hd44780_add_new_vt100_param(lcd);
        }

    case CMD_END:
        if(!lcd->esc_seq_buf.param_err )
        {
            switch(ch)
            {
            case 'J':
                if(lcd->esc_seq_buf.param_index == 1 && lcd->esc_seq_buf.param[0] == 2)
                {
                    hd44780_clear_display(lcd);
                    hd44780_write_instruction(lcd, HD44780_RETURN_HOME);
                    lcd->pos.row = 0;
                    lcd->pos.col = 0;
                }
                break;

            case 'H':
                if(lcd->esc_seq_buf.param_index == 2 )
                    hd44780_handle_setcursor(lcd, lcd->esc_seq_buf.param[0], lcd->esc_seq_buf.param[1]);
                break;
            }
        }
        VT100_state = CMD_START;
        hd44780_leave_esc_seq(lcd);
        break;
    }
}


void hd44780_write(struct hd44780 *lcd, const char *buf, size_t count)
{
    size_t i;
    char ch;

    if (lcd->dirty) {
        hd44780_clear_display(lcd);
        lcd->dirty = false;
    }
    if(!lcd->cursor_blink && !lcd->cursor_display && lcd->newline_dirty) {
        lcd->newline_dirty = false;
        hd44780_handle_new_line(lcd);
    }

    for (i = 0; i < count; i++) {
        ch = buf[i];

        if (lcd->is_in_esc_seq) {
            hd44780_handle_esc_seq_char(lcd, ch);
        } else {
            switch (ch) {
            case '\r':
                hd44780_handle_carriage_return(lcd);
                break;
            case '\n':
                if (!lcd->cursor_blink && !lcd->cursor_display && i != count-1)
                    hd44780_handle_new_line(lcd);
                else
                    lcd->newline_dirty = true;
                break;
            case '\e':
                lcd->is_in_esc_seq = true;
                break;
            case '\t':
                hd44780_handle_tab(lcd);
                break;
            default:
                hd44780_write_char(lcd, ch);
                break;
            }
        }
    }
}

void hd44780_print(struct hd44780 *lcd, const char *str)
{
    hd44780_write(lcd, str, strlen(str));
}

void hd44780_set_geometry(struct hd44780 *lcd, struct hd44780_geometry *geo)
{
    lcd->geometry = geo;

    if (lcd->is_in_esc_seq)
        hd44780_leave_esc_seq(lcd);

    hd44780_clear_display(lcd);
}

void hd44780_set_backlight(struct hd44780 *lcd, bool backlight)
{
    lcd->backlight = backlight;
    pcf8574_raw_write(lcd, backlight ? BL : 0x00);
}

static void hd44780_update_display_ctrl(struct hd44780 *lcd)
{

    hd44780_write_instruction(lcd, HD44780_DISPLAY_CTRL
                              | HD44780_D_DISPLAY_ON
                              | (lcd->cursor_display ? HD44780_C_CURSOR_ON
                                                     : HD44780_C_CURSOR_OFF)
                              | (lcd->cursor_blink ? HD44780_B_BLINK_ON
                                                   : HD44780_B_BLINK_OFF));
}

void hd44780_set_cursor_blink(struct hd44780 *lcd, bool cursor_blink)
{
    lcd->cursor_blink = cursor_blink;
    hd44780_update_display_ctrl(lcd);
}

void hd44780_set_cursor_display(struct hd44780 *lcd, bool cursor_display)
{
    lcd->cursor_display= cursor_display;
    hd44780_update_display_ctrl(lcd);
}
void hd44780_init_lcd(struct hd44780 *lcd)
{
    lcd->esc_seq_buf.param_index = 0;
    lcd->esc_seq_buf.param_err = 0;
    hd44780_write_instruction_high_nibble(lcd, HD44780_FUNCTION_SET
                                          | HD44780_DL_8BITS);
    mdelay(5);

    hd44780_write_instruction_high_nibble(lcd, HD44780_FUNCTION_SET
                                          | HD44780_DL_8BITS);
    udelay(100);

    hd44780_write_instruction_high_nibble(lcd, HD44780_FUNCTION_SET
                                          | HD44780_DL_8BITS);

    hd44780_write_instruction_high_nibble(lcd, HD44780_FUNCTION_SET
                                          | HD44780_DL_4BITS);

    hd44780_write_instruction(lcd, HD44780_FUNCTION_SET | HD44780_DL_4BITS
                              | HD44780_N_2LINES);

    hd44780_write_instruction(lcd, HD44780_DISPLAY_CTRL | HD44780_D_DISPLAY_ON
                              | HD44780_C_CURSOR_ON | HD44780_B_BLINK_ON);

    hd44780_clear_display(lcd);

    hd44780_write_instruction(lcd, HD44780_ENTRY_MODE_SET
                              | HD44780_ID_INCREMENT | HD44780_S_SHIFT_OFF);
}
