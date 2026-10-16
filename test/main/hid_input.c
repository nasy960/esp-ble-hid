#include "hid_input.h"
#include <string.h>
#include <limits.h>

enum { KEY_ARRAY = 1, KEY_BIT, BUTTON, AXIS_X, AXIS_Y, WHEEL, PAN };
typedef struct {
    uint32_t page, size, count;
    int32_t minimum;
    uint8_t id;
} globals_t;

static uint32_t unsigned_value(const uint8_t *p, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static int32_t signed_value(uint32_t v, unsigned bits)
{
    if (bits == 32) return (int32_t)v;
    if (bits && (v & (1u << (bits - 1)))) v |= UINT32_MAX << bits;
    return (int32_t)v;
}
static int report_index(hid_layout_t *out, uint8_t id)
{
    for (unsigned i = 0; i < out->reports; ++i) if (out->ids[i] == id) return i;
    if (out->reports == HID_LAYOUT_REPORTS) return -1;
    unsigned r = out->reports++;
    out->ids[r] = id;
    return r;
}
static bool add_field(hid_layout_t *o, int r, unsigned off, globals_t g,
                      unsigned count, unsigned kind, unsigned usage)
{
    if (o->fields == HID_LAYOUT_FIELDS) return false;
    o->field[o->fields++] = (hid_field_t) {
        .offset = off, .count = count, .size = g.size, .report = r,
        .kind = kind, .usage = usage, .logical_min = g.minimum,
    };
    return true;
}

bool hid_layout_parse(hid_layout_t *out, const uint8_t *desc, size_t len)
{
    memset(out, 0, sizeof(*out));
    globals_t g = {0}, stack[8];
    unsigned depth = 0, collections = 0, usage_count = 0;
    uint32_t usages[256], usage_min = 0, usage_max = 0;
    bool has_min = false, has_max = false;
    for (size_t p = 0; p < len;) {
        uint8_t tag = desc[p++];
        if (tag == 0xfe) return false; // Long items are not supported.
        unsigned size = (tag & 3) == 3 ? 4 : tag & 3;
        unsigned type = (tag >> 2) & 3, item = tag >> 4;
        if (len - p < size) return false;
        uint32_t v = unsigned_value(desc + p, size);
        p += size;
        if (type == 1) {
            switch (item) {
            case 0: g.page = v; break;
            case 1: g.minimum = signed_value(v, size * 8); break;
            case 7: g.size = v; break;
            case 8:
                if (v == 0 || v > 255) return false;
                g.id = v; out->has_ids = true; break;
            case 9: g.count = v; break;
            case 10:
                if (depth == 8) return false;
                stack[depth++] = g; break;
            case 11:
                if (!depth) return false;
                g = stack[--depth]; break;
            default: break;
            }
        } else if (type == 2) {
            uint32_t usage = size == 4 ? v : (g.page << 16) | v;
            switch (item) {
            case 0:
                if (usage_count == 256) return false;
                usages[usage_count++] = usage; break;
            case 1: usage_min = usage; has_min = true; break;
            case 2: usage_max = usage; has_max = true; break;
            case 10: return false; // Usage delimiters require alternate-set handling.
            default: break;
            }
        } else if (type == 0) {
            if (item == 10) ++collections;
            if (item == 12) {
                if (!collections) return false;
                --collections;
            }
            if (item == 9) { // Output bit positions are independent of Input.
                if (!g.size || g.size > 32 || !g.count || g.count > 512) return false;
                int r = report_index(out, g.id);
                if (r < 0) return false;
                unsigned offset = out->output_bits[r], bits = g.size*g.count;
                if (offset+bits > (HID_INPUT_BYTES-1)*8) return false;
                out->output_bits[r] += bits;
                if (!(v&1)) for (unsigned i=0;i<g.count;++i) {
                    uint32_t u = usage_count ? usages[i<usage_count ? i : usage_count-1] :
                        has_min && has_max && usage_max>=usage_min ?
                        usage_min+(i<usage_max-usage_min ? i : usage_max-usage_min) : 0;
                    unsigned usage = u&0xffff;
                    if ((v&2) && g.size==1 && (u>>16)==8 && usage>=1 && usage<=5)
                        out->led_offset[r][usage-1] = offset+i+1;
                    else out->output_other[r] = true; // Don't zero unrelated output controls.
                }
            }
            if (item == 8) {
                if (!g.size || g.size > 32 || !g.count || g.count > 512) return false;
                int r = report_index(out, g.id);
                if (r < 0) return false;
                unsigned offset = out->bits[r], bits = g.size * g.count;
                if (offset + bits > HID_INPUT_BYTES * 8) return false;
                out->bits[r] += bits;
                if (!(v & 1)) { // Constants still consume bits.
                    if (!(v & 2)) {
                        if (g.page == 7 &&
                            !add_field(out, r, offset, g, g.count, KEY_ARRAY, 0)) return false;
                    } else {
                        for (unsigned i = 0; i < g.count; ++i) {
                            uint32_t u = 0;
                            if (usage_count) u = usages[i < usage_count ? i : usage_count - 1];
                            else if (has_min && has_max && usage_max >= usage_min)
                                u = usage_min + (i < usage_max - usage_min ? i : usage_max - usage_min);
                            unsigned page = u >> 16, usage = u & 0xffff, kind = 0;
                            if (page == 7 && usage < 256) kind = KEY_BIT;
                            if (page == 9 && usage >= 1 && usage <= 8) kind = BUTTON;
                            if (v & 4) { // Only relative mouse motion is supported.
                                if (page == 1 && usage == 0x30) kind = AXIS_X;
                                if (page == 1 && usage == 0x31) kind = AXIS_Y;
                                if (page == 1 && usage == 0x38) kind = WHEEL;
                                if (page == 0x0c && usage == 0x238) kind = PAN;
                            }
                            if (kind && !add_field(out, r, offset + i * g.size, g, 1, kind, usage)) return false;
                        }
                    }
                }
            }
            usage_count = 0; has_min = has_max = false;
        }
    }
    if (depth || collections || !out->fields) return false;
    for (unsigned i = 0; i < out->reports; ++i) {
        if (out->has_ids && !out->ids[i]) return false;
        if (out->bits[i] + (out->has_ids ? 8 : 0) > HID_INPUT_BYTES * 8) return false;
    }
    return true;
}

static uint32_t read_bits(const uint8_t *p, unsigned off, unsigned bits)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < bits; ++i)
        v |= (uint32_t)((p[(off + i) / 8] >> ((off + i) % 8)) & 1) << i;
    return v;
}
bool hid_layout_decode(const hid_layout_t *layout, const uint8_t *data,
                       size_t len, hid_input_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!len) return false;
    uint8_t id = layout->has_ids ? *data++ : 0;
    if (layout->has_ids) --len;
    unsigned r;
    for (r = 0; r < layout->reports && layout->ids[r] != id; ++r) {}
    if (r == layout->reports || len * 8 < layout->bits[r]) return false;
    out->report = r;
    for (unsigned i = 0; i < layout->fields; ++i) {
        const hid_field_t *f = &layout->field[i];
        if (f->report != r) continue;
        if (f->kind == KEY_ARRAY || f->kind == KEY_BIT) out->keyboard = true;
        else out->mouse = true;
        for (unsigned k = 0; k < f->count; ++k) {
            uint32_t value = read_bits(data, f->offset + k * f->size, f->size);
            int32_t s = f->logical_min < 0 ? signed_value(value, f->size) : (int32_t)value;
            switch (f->kind) {
            case KEY_ARRAY:
                // Preserve previous keyboard state on rollover/error reports.
                if (value >= 1 && value <= 3) return false;
                if (value > 3 && value < 256) out->keys[value / 8] |= 1u << (value % 8);
                break;
            case KEY_BIT:
                if (value && f->usage > 3) out->keys[f->usage / 8] |= 1u << (f->usage % 8);
                break;
            case BUTTON: if (value) out->buttons |= 1u << (f->usage - 1); break;
            case AXIS_X: out->x = s; break;
            case AXIS_Y: out->y = s; break;
            case WHEEL: out->wheel = s; break;
            case PAN: out->pan = s; break;
            }
        }
    }
    return out->keyboard || out->mouse;
}

size_t hid_layout_led_report(const hid_layout_t *layout, unsigned report, uint8_t leds, uint8_t data[HID_INPUT_BYTES])
{
    if (report >= layout->reports || layout->output_other[report]) return 0;
    bool found = false;
    memset(data,0,HID_INPUT_BYTES);
    unsigned prefix = layout->has_ids ? 1 : 0;
    if (prefix) data[0] = layout->ids[report];
    for(unsigned i=0;i<5;++i) if(layout->led_offset[report][i]) {
        found = true;
        unsigned bit = layout->led_offset[report][i]-1;
        if(leds & (1u<<i)) data[prefix+bit/8] |= 1u<<(bit%8);
    }
    return found ? prefix+(layout->output_bits[report]+7)/8 : 0;
}
