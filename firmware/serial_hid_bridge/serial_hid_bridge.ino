#include <Keyboard.h>

struct KeyMap {
  byte vk;
  byte key;
};

const KeyMap SPECIAL_KEYS[] = {
  {0x08, KEY_BACKSPACE},
  {0x09, KEY_TAB},
  {0x0D, KEY_RETURN},
  {0x10, KEY_LEFT_SHIFT},
  {0x11, KEY_LEFT_CTRL},
  {0x12, KEY_LEFT_ALT},
  {0x1B, KEY_ESC},
  {0x20, ' '},
  {0x21, KEY_PAGE_UP},
  {0x22, KEY_PAGE_DOWN},
  {0x23, KEY_END},
  {0x24, KEY_HOME},
  {0x25, KEY_LEFT_ARROW},
  {0x26, KEY_UP_ARROW},
  {0x27, KEY_RIGHT_ARROW},
  {0x28, KEY_DOWN_ARROW},
  {0x2E, KEY_DELETE},
  {0x70, KEY_F1},
  {0x71, KEY_F2},
  {0x72, KEY_F3},
  {0x73, KEY_F4},
  {0x74, KEY_F5},
  {0x75, KEY_F6},
  {0x76, KEY_F7},
  {0x77, KEY_F8},
  {0x78, KEY_F9},
  {0x79, KEY_F10},
  {0x7A, KEY_F11},
  {0x7B, KEY_F12}
};

byte keyForVirtualKey(byte vk) {
  if (vk >= 'A' && vk <= 'Z') {
    return 'a' + (vk - 'A');
  }
  if (vk >= '0' && vk <= '9') {
    return vk;
  }

  for (unsigned i = 0; i < sizeof(SPECIAL_KEYS) / sizeof(SPECIAL_KEYS[0]); ++i) {
    if (SPECIAL_KEYS[i].vk == vk) {
      return SPECIAL_KEYS[i].key;
    }
  }

  return 0;
}

int hexValue(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return 10 + ch - 'a';
  }
  if (ch >= 'A' && ch <= 'F') {
    return 10 + ch - 'A';
  }
  return -1;
}

void setup() {
  Serial.begin(115200);
  Keyboard.begin();
}

void loop() {
  if (!Serial.available()) {
    return;
  }

  const char action = Serial.read();
  if (action != 'D' && action != 'U') {
    return;
  }

  while (Serial.available() && Serial.peek() == ' ') {
    Serial.read();
  }

  while (Serial.available() < 2) {
    delay(1);
  }

  const int hi = hexValue(Serial.read());
  const int lo = hexValue(Serial.read());
  if (hi < 0 || lo < 0) {
    return;
  }

  const byte key = keyForVirtualKey((hi << 4) | lo);
  if (key == 0) {
    return;
  }

  if (action == 'D') {
    Keyboard.press(key);
  } else {
    Keyboard.release(key);
  }
}
