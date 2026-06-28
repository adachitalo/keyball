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

#define SPEED_PCT    25   // 速度係数(縮小率) ×100。低速の基準速度。下げると全体的に遅く=精密
#define EXPO_MAX   2000   // 高速時のexpoゲイン ×100（実効最大 = SPEED_PCT×EXPO_MAX/100 = 0.25×20 = 5倍）
#define EXPO_CURVE    2   // expoカーブの鋭さ（整数: 2=中盤が持ち上がる / 3=中央が鈍くexpo風）
#define EXPO_REF     80   // 「全速」とみなす1レポートの移動量（小さいほど早く最大加速に到達）

static uint32_t expo_lut[128];   // a×expoゲイン ×256（unityスケール）
static int32_t  carry_x, carry_y;

// 動き始めランプ（時間ベース）：トラックボールの"周り始めの硬さ"で出る初動スパイクを抑える。
// 停止→動き出しの最初だけゲインを絞り、ONSET_MS かけて通常へ戻す。報告レートに依らず一定時間。
#define ONSET_MS    120   // 動き始めにこの時間[ms]かけて通常ゲインへ立ち上げる（大=ゆっくり立上り）
#define ONSET_MIN    25   // 動き始めの倍率 ×100（小さいほど初動を強く抑える, 100=ランプ無効）
#define ONSET_GAP_MS 80   // この時間[ms]以上停止したら「動き始め」とみなしランプ再武装
static uint32_t onset_start, onset_last;

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

static int16_t speed_axis(int16_t v, int32_t *carry, int16_t rgain) {
    int16_t a = v < 0 ? -v : v;
    if (a > 127) a = 127;                    // 拡張レポートの大入力は上限で頭打ち（64bit回避＝省フラッシュ）
    int32_t out256 = (int32_t)expo_lut[a] * SPEED_PCT / 100;   // 縮小係数を適用
    out256 = out256 * rgain / 100;          // 動き始めランプ（初動スパイク抑制）
    if (v < 0) out256 = -out256;
    *carry += out256;                       // 端数を蓄積
    int16_t whole = (int16_t)(*carry / 256);
    *carry -= (int32_t)whole * 256;         // 端数を次回へ持ち越し
    return whole;
}

report_mouse_t pointing_device_task_user(report_mouse_t mouse_report) {
    // ※OLEDの「Ball:」行 x/y はこの変換の"前"の値＝ほぼ生のセンサー移動量。実測に使える。
    if (!keyball_get_scroll_mode()) {        // スクロール中はポインター曲線は適用しない（スクロールは純正）
        int16_t rgain = 100;
        if (mouse_report.x != 0 || mouse_report.y != 0) {
            uint32_t now = timer_read32();
            if (TIMER_DIFF_32(now, onset_last) > ONSET_GAP_MS) onset_start = now;  // 久々の動き=立上り開始
            onset_last = now;
            uint32_t since = TIMER_DIFF_32(now, onset_start);
            if (since > ONSET_MS) since = ONSET_MS;
            rgain = ONSET_MIN + (int32_t)(100 - ONSET_MIN) * since / ONSET_MS;
        }
        mouse_report.x = speed_axis(mouse_report.x, &carry_x, rgain);
        mouse_report.y = speed_axis(mouse_report.y, &carry_y, rgain);
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
