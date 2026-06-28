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
#include <math.h>

#define SPEED_PCT    33   // 速度係数(縮小率) ×100。CPIを3倍にしたなら33で従来比≒等速。下げると遅く=精密
#define EXPO_MAX    100   // 高速時の追加倍率 ×100（100=加速なし/最高速を抑える, 上げると速い動きを加速）
#define EXPO_CURVE    3   // expoカーブの鋭さ（EXPO_MAX>100のとき有効）
#define EXPO_REF     40   // 「全速」とみなす1レポートあたりの移動量

static uint32_t expo_lut[128];   // a×expoゲイン ×256（unityスケール）
static int32_t  carry_x, carry_y;

void keyboard_post_init_user(void) {
    for (uint16_t i = 0; i < 128; i++) {
        float t = (float)i / (float)EXPO_REF;
        if (t > 1.0f) t = 1.0f;
        float gain = 100.0f + ((float)EXPO_MAX - 100.0f) * powf(t, (float)EXPO_CURVE);
        expo_lut[i] = (uint32_t)((float)i * gain / 100.0f * 256.0f + 0.5f);
    }
}

static int16_t speed_axis(int16_t v, int32_t *carry) {
    int16_t a = v < 0 ? -v : v;
    int64_t base256 = (a < 128) ? (int64_t)expo_lut[a]
                                : ((int64_t)a * EXPO_MAX * 256) / 100;
    int32_t out256 = (int32_t)(base256 * SPEED_PCT / 100);   // 縮小係数を適用
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
