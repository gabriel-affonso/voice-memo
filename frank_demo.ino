#include <Arduino.h>
#include "frank_sprites.h"

// Substitua as chamadas de desenho/refresh pelas da biblioteca
// usada no firmware do ESP32-S3-Touch-ePaper-1.54.

void showFrank(FrankState s) {
  // Exemplo conceitual:
  // display.fillScreen(WHITE);
  // display.drawBitmap(0, 0, frankFullSprite(s), 200, 200, BLACK);
  // display.display(false); // full refresh
}

void bootAnimation() {
  showFrank(FRANK_CLOSED);
  delay(300);

  showFrank(FRANK_HALF_OPEN);
  delay(300);

  showFrank(FRANK_AWAKE);
  delay(350);

  showFrank(FRANK_BLINK);
  delay(220);

  showFrank(FRANK_AWAKE);
}

void setup() {
  // init display
  bootAnimation();
}

void loop() {
}
