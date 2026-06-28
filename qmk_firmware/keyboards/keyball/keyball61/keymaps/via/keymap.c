/*
Copyright 2022 @Yowkees
Copyright 2022 MURAOKA Taro (aka KoRoN, @kaoriya)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include QMK_KEYBOARD_H

#include "quantum.h"

// --- 押下中だけ低CPI（精密モード） ---------------------------------------
enum custom_keycodes {
    CPI_PREC = KEYBALL_SAFE_RANGE,   // 押下中だけ低CPIにする精密モードキー（= VIA USER00 / 0x7E40）
};

#define PRECISION_CPI 4              // 押下中のCPI（×100単位 → 4 = 400CPI）

static uint8_t saved_cpi = 0;

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case CPI_PREC:
            if (record->event.pressed) {
                saved_cpi = keyball_get_cpi();   // 現在のCPIを退避
                keyball_set_cpi(PRECISION_CPI);  // 精密モードへ
            } else {
                keyball_set_cpi(saved_cpi);      // 離したら元のCPIへ復帰
            }
            return false;
    }
    return true;
}
// ------------------------------------------------------------------------

// --- ポインター速度制御：高解像度(高CPI) + 縮小係数 + 端数キャリー ----------
// 考え方：
//  - 解像度は CPI を上げて稼ぐ（例：従来500の3倍 = 1500CPI を実機で設定する）。
//  - その移動量にファーム側で縮小係数 SPEED_PCT を掛けてカーソル速度を決める。
//    例: CPI3倍 × SPEED_PCT=33% ≒ 従来と同等の速度。係数を下げれば更に遅く=精密。
//  - 縮小で出る端数は次レポートへ持ち越す（キャリー）ので低速の微小移動が消えない。
//    → 同じ速度でも高解像度ぶん滑らかで細かく狙える。
//  - EXPO_MAX を 100 超にすると、速い動きだけ追加で加速（任意・既定はOFF）。
// 数字は感覚で調整可。変更したら再ビルドするだけ。
// ※ floatは使わない（ATmega32U4はフラッシュが厳しいため整数のみで計算）。

#define SPEED_PCT    33   // 速度係数(縮小率) ×100。CPIを3倍にしたなら33で従来比≒等速。下げると遅く=精密
#define EXPO_MAX    100   // 高速時の追加倍率 ×100（100=加速なし/最高速を抑える, 100超で速い動きを加速）
#define EXPO_CURVE    3   // expoカーブの鋭さ（整数: 2 or 3。EXPO_MAX>100のとき有効）
#define EXPO_REF     40   // 「全速」とみなす1レポートあたりの移動量

static uint32_t expo_lut[128];   // a×expoゲイン ×256（unityスケール）
static int32_t  carry_x, carry_y;

void keyboard_post_init_user(void) {
    uint32_t ref_pow = 1;
    for (uint8_t c = 0; c < EXPO_CURVE; c++) ref_pow *= EXPO_REF;
    for (uint16_t i = 0; i < 128; i++) {
        uint32_t ip   = (i < EXPO_REF) ? i : EXPO_REF;   // i/EXPO_REF を 1.0 で頭打ち
        uint32_t inum = 1;
        for (uint8_t c = 0; c < EXPO_CURVE; c++) inum *= ip;
        uint32_t gain100 = 100 + (uint32_t)(EXPO_MAX - 100) * inum / ref_pow;  // ×100, 100=等倍
        expo_lut[i] = (uint32_t)i * 256 * gain100 / 100;
    }
}

static int16_t speed_axis(int16_t v, int32_t *carry) {
    int16_t a = v < 0 ? -v : v;
    if (a > 127) a = 127;                    // 拡張レポートの大入力は上限で頭打ち（64bit回避＝省フラッシュ）
    int32_t out256 = (int32_t)expo_lut[a] * SPEED_PCT / 100;   // 縮小係数を適用
    if (v < 0) out256 = -out256;
    *carry += out256;                       // 端数を蓄積
    int16_t whole = (int16_t)(*carry / 256);
    *carry -= (int32_t)whole * 256;         // 端数を次回へ持ち越し
    return whole;
}

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    if (!keyball_get_scroll_mode()) {        // スクロール中は適用しない
        mouse_report.x = speed_axis(mouse_report.x, &carry_x);
        mouse_report.y = speed_axis(mouse_report.y, &carry_y);
    }
    return mouse_report;
}
// ------------------------------------------------------------------------

// --- スクロールの加速 / expo ----------------------------------------------
// Keyball純正のスクロール変換フックを上書きし、生のボール移動量にカーブを掛ける。
// ゆっくり回す=1ノッチ精密、速く回す=一気にスクロール（加速）。端数はキャリーで保持。
// 分割(左右)・スクロールスナップ(縦/横ロック)は純正どおり維持。
#define SCRL_DIV     48   // 基本の重さ（大きいほどゆっくり=精密）
#define SCRL_MAX    400   // 速いスピン時の最大倍率 ×100（100=加速なし, 400=最大4倍）
#define SCRL_REF     16   // 「全速スピン」とみなす1レポートの生移動量（小さいほど早く最大加速）

static int32_t scrl_acc_h, scrl_acc_v;

static int32_t scrl_scale(int16_t mv) {           // 返り値は ×256 固定小数
    int16_t a = mv < 0 ? -mv : mv;
    if (a > 255) a = 255;                          // 桁あふれ防止（速い動きは頭打ち）
    int32_t ip   = a < SCRL_REF ? a : SCRL_REF;    // i/REF を 1.0 で頭打ち
    int32_t gain = 100 + (int32_t)(SCRL_MAX - 100) * ip * ip / ((int32_t)SCRL_REF * SCRL_REF);
    int32_t out  = (int32_t)a * gain * 256 / 100 / SCRL_DIV;
    return mv < 0 ? -out : out;
}

static int8_t scrl_emit(int32_t scaled, int32_t *acc) {
    *acc += scaled;
    int32_t w = *acc / 256;
    *acc -= w * 256;                               // 端数を次回へ持ち越し
    if (w >  127) w =  127;
    if (w < -127) w = -127;
    return (int8_t)w;
}

void keyball_on_apply_motion_to_mouse_scroll(keyball_motion_t *m, report_mouse_t *r, bool is_left) {
    if (m->x == 0 && m->y == 0) { r->h = 0; r->v = 0; return; }
    int32_t sh =  scrl_scale(m->y);               // Keyball61: h <- y, v <- -x
    int32_t sv = -scrl_scale(m->x);
    m->x = 0; m->y = 0;                            // 生移動量を消費（端数は自前のキャリーで保持）
    int8_t h = scrl_emit(sh, &scrl_acc_h);
    int8_t v = scrl_emit(sv, &scrl_acc_v);
    if (is_left) { h = -h; v = -v; }
    r->h = h;
    r->v = v;
    switch (keyball_get_scrollsnap_mode()) {       // スナップ（縦/横ロック）を純正どおり維持
        case KEYBALL_SCROLLSNAP_MODE_VERTICAL:   r->h = 0; break;
        case KEYBALL_SCROLLSNAP_MODE_HORIZONTAL: r->v = 0; break;
        default: break;
    }
}
// ------------------------------------------------------------------------

// clang-format off
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
  [0] = LAYOUT_universal(
    KC_ESC   , KC_1     , KC_2     , KC_3     , KC_4     , KC_5     ,                                  KC_6     , KC_7     , KC_8     , KC_9     , KC_0     , KC_MINS  ,
    KC_DEL   , KC_Q     , KC_W     , KC_E     , KC_R     , KC_T     ,                                  KC_Y     , KC_U     , KC_I     , KC_O     , KC_P     , KC_INT3  ,
    KC_TAB   , KC_A     , KC_S     , KC_D     , KC_F     , KC_G     ,                                  KC_H     , KC_J     , KC_K     , KC_L     , KC_SCLN  , S(KC_7)  ,
    MO(1)    , KC_Z     , KC_X     , KC_C     , KC_V     , KC_B     , KC_RBRC  ,              KC_NUHS, KC_N     , KC_M     , KC_COMM  , KC_DOT   , KC_SLSH  , KC_RSFT  ,
    _______  , KC_LCTL  , KC_LALT  , KC_LGUI,LT(1,KC_LNG2),LT(2,KC_SPC),LT(3,KC_LNG1),    KC_BSPC,LT(2,KC_ENT),LT(1,KC_LNG2),KC_RGUI, _______ , KC_RALT  , KC_PSCR
  ),

  [1] = LAYOUT_universal(
    S(KC_ESC), S(KC_1)  , KC_LBRC  , S(KC_3)  , S(KC_4)  , S(KC_5)  ,                                  KC_EQL   , S(KC_6)  ,S(KC_QUOT), S(KC_8)  , S(KC_9)  ,S(KC_INT1),
    S(KC_DEL), S(KC_Q)  , S(KC_W)  , S(KC_E)  , S(KC_R)  , S(KC_T)  ,                                  S(KC_Y)  , S(KC_U)  , S(KC_I)  , S(KC_O)  , S(KC_P)  ,S(KC_INT3),
    S(KC_TAB), S(KC_A)  , S(KC_S)  , S(KC_D)  , S(KC_F)  , S(KC_G)  ,                                  S(KC_H)  , S(KC_J)  , S(KC_K)  , S(KC_L)  , KC_QUOT  , S(KC_2)  ,
    _______  , S(KC_Z)  , S(KC_X)  , S(KC_C)  , S(KC_V)  , S(KC_B)  ,S(KC_RBRC),           S(KC_NUHS), S(KC_N)  , S(KC_M)  ,S(KC_COMM), S(KC_DOT),S(KC_SLSH),S(KC_RSFT),
    _______  ,S(KC_LCTL),S(KC_LALT),S(KC_LGUI), _______  , _______  , _______  ,            _______  , _______  , _______  ,S(KC_RGUI), _______  , S(KC_RALT), _______
  ),

  [2] = LAYOUT_universal(
    SSNP_FRE , KC_F1    , KC_F2    , KC_F3    , KC_F4    , KC_F5    ,                                  KC_F6    , KC_F7    , KC_F8    , KC_F9    , KC_F10   , KC_F11   ,
    SSNP_VRT , _______  , KC_7     , KC_8     , KC_9     , _______  ,                                  _______  , KC_LEFT  , KC_UP    , KC_RGHT  , _______  , KC_F12   ,
    SSNP_HOR , _______  , KC_4     , KC_5     , KC_6     ,S(KC_SCLN),                                  KC_PGUP  , KC_BTN1  , KC_DOWN  , KC_BTN2  , KC_BTN3  , _______  ,
    _______  , _______  , KC_1     , KC_2     , KC_3     ,S(KC_MINS), S(KC_8)  ,            S(KC_9)  , KC_PGDN  , _______  , _______  , _______  , _______  , _______  ,
    _______  , _______  , KC_0     , KC_DOT   , _______  , _______  , _______  ,             KC_DEL  , _______  , _______  , _______  , _______  , _______  , _______
  ),

  [3] = LAYOUT_universal(
    RGB_TOG  , AML_TO   , AML_I50  , AML_D50  , _______  , _______  ,                                  RGB_M_P  , RGB_M_B  , RGB_M_R  , RGB_M_SW , RGB_M_SN , RGB_M_K  ,
    RGB_MOD  , RGB_HUI  , RGB_SAI  , RGB_VAI  , _______  , _______  ,                                  RGB_M_X  , RGB_M_G  , RGB_M_T  , RGB_M_TW , _______  , _______  ,
    RGB_RMOD , RGB_HUD  , RGB_SAD  , RGB_VAD  , _______  , _______  ,                                  CPI_D1K  , CPI_D100 , CPI_I100 , CPI_I1K  , KBC_SAVE , KBC_RST  ,
    _______  , _______  , SCRL_DVD , SCRL_DVI , SCRL_MO  , SCRL_TO  , EE_CLR   ,            EE_CLR   , KC_HOME  , KC_PGDN  , KC_PGUP  , KC_END   , _______  , _______  ,
    QK_BOOT  , _______  , KC_LEFT  , KC_DOWN  , KC_UP    , KC_RGHT  , _______  ,            _______  , KC_BSPC  , _______  , _______  , _______  , _______  , QK_BOOT
  ),
};
// clang-format on

layer_state_t layer_state_set_user(layer_state_t state) {
    // Auto enable scroll mode when the highest layer is 3
    keyball_set_scroll_mode(get_highest_layer(state) == 3);
    return state;
}

#ifdef OLED_ENABLE

#    include "lib/oledkit/oledkit.h"

void oledkit_render_info_user(void) {
    keyball_oled_render_keyinfo();
    keyball_oled_render_ballinfo();
    keyball_oled_render_layerinfo();
}
#endif
