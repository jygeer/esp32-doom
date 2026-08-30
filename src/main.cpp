// ESP32 "Cheap Yellow Display" raycaster FPS -- a Doom/Wolfenstein-style clone.
// Display: ILI9341 320x240 via TFT_eSPI (VSPI). Touch: XPT2046 resistive via a
// separate HSPI bus (CYD wires touch on different pins than the panel).
#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <math.h>
#include <esp_heap_caps.h>

// ---------- Touch pins (CYD: touch is on its own bus, not the display's) ----------
// Bit-banged here (not TFT_eSPI's hardware SPI bus) so it never contends with
// the display's VSPI transactions.
#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_CLK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite fb = TFT_eSprite(&tft); // full-frame back buffer: draw here, push once per frame
Preferences prefs;

void touchBegin() {
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_CLK, OUTPUT);
  digitalWrite(TOUCH_CLK, LOW);
  pinMode(TOUCH_MOSI, OUTPUT);
  pinMode(TOUCH_MISO, INPUT);
  pinMode(TOUCH_IRQ, INPUT);
}

uint8_t touchXferByte(uint8_t out) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TOUCH_MOSI, (out >> i) & 1);
    delayMicroseconds(2);
    digitalWrite(TOUCH_CLK, HIGH);
    in <<= 1;
    if (digitalRead(TOUCH_MISO)) in |= 1;
    delayMicroseconds(2);
    digitalWrite(TOUCH_CLK, LOW);
  }
  return in;
}

uint16_t touchReadChannel(uint8_t cmd) {
  digitalWrite(TOUCH_CS, LOW);
  touchXferByte(cmd);
  uint16_t hi = touchXferByte(0x00);
  uint16_t lo = touchXferByte(0x00);
  digitalWrite(TOUCH_CS, HIGH);
  return ((hi << 8) | lo) >> 3 & 0x0FFF;
}

bool touchIsTouched() {
  return digitalRead(TOUCH_IRQ) == LOW;
}

// Command bytes: 0xD0 = X channel, 0x90 = Y channel (standard XPT2046 wiring).
bool touchReadRaw(int &x, int &y) {
  if (!touchIsTouched()) return false;
  touchReadChannel(0xD0); // throwaway, lets the ADC settle
  int rx = touchReadChannel(0xD0);
  int ry = touchReadChannel(0x90);
  if (!touchIsTouched()) return false;
  x = rx; y = ry;
  return true;
}

// ---------- Screen ----------
static const int SCREEN_W = 320;
static const int SCREEN_H = 240;
// The ESP32 (no PSRAM) can't reliably get one contiguous 150KB block for a
// full-frame sprite once the heap is fragmented, so we render in horizontal
// bands into a small sprite and push each band to the display in turn. The
// largest available contiguous block varies with heap fragmentation, so pick
// the biggest band height (evenly dividing SCREEN_H) that actually allocates.
int gBandH = 40;

// ---------- Touch calibration (3-point, learned at boot) ----------
// A 2-point diagonal calibration can't tell a plain scale/offset from an
// axis swap (raw X wired to screen Y or vice versa). Three points where each
// pair varies only one screen axis lets us detect and correct a swap too.
struct Calib { bool swapped; int xLo, xHi, yLo, yHi; bool valid; } cal;

bool sampleRawTouch(int &rx, int &ry) {
  long sx = 0, sy = 0;
  int n = 0;
  for (int i = 0; i < 6; i++) {
    int px, py;
    if (!touchReadRaw(px, py)) break;
    sx += px; sy += py; n++;
    delay(4);
  }
  if (n == 0) return false;
  rx = sx / n; ry = sy / n;
  return true;
}

void drawCrosshair(int x, int y, uint16_t c) {
  tft.drawLine(x - 10, y, x + 10, y, c);
  tft.drawLine(x, y - 10, x, y + 10, c);
  tft.drawCircle(x, y, 6, c);
}

int calTargetX[3], calTargetY[3];
int calRawX[3], calRawY[3];

void calibrationTapTarget(int idx, int x, int y) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("TOUCH THE TARGET " + String(idx + 1) + "/3", SCREEN_W / 2, 20, 2);
  drawCrosshair(x, y, TFT_YELLOW);

  int rx, ry;
  while (!sampleRawTouch(rx, ry)) delay(10);
  calRawX[idx] = rx; calRawY[idx] = ry;
  while (touchIsTouched()) delay(10);
  delay(150);
}

void runCalibration() {
  // A: top-left, B: top-right (screen X varies, Y fixed), C: bottom-left (screen Y varies, X fixed).
  calTargetX[0] = 30;             calTargetY[0] = 30;
  calTargetX[1] = SCREEN_W - 30;  calTargetY[1] = 30;
  calTargetX[2] = 30;             calTargetY[2] = SCREEN_H - 30;

  for (int i = 0; i < 3; i++) calibrationTapTarget(i, calTargetX[i], calTargetY[i]);

  int dABx = calRawX[1] - calRawX[0];
  int dABy = calRawY[1] - calRawY[0];
  cal.swapped = abs(dABy) > abs(dABx);

  if (!cal.swapped) {
    cal.xLo = calRawX[0]; cal.xHi = calRawX[1];
    cal.yLo = calRawY[0]; cal.yHi = calRawY[2];
  } else {
    cal.xLo = calRawY[0]; cal.xHi = calRawY[1];
    cal.yLo = calRawX[0]; cal.yHi = calRawX[2];
  }

  cal.valid = true;
  prefs.putBool("swapped", cal.swapped);
  prefs.putInt("xLo", cal.xLo);
  prefs.putInt("xHi", cal.xHi);
  prefs.putInt("yLo", cal.yLo);
  prefs.putInt("yHi", cal.yHi);
  prefs.putBool("valid", true);
}

bool getTouchScreen(int &sx, int &sy) {
  int rx, ry;
  if (!sampleRawTouch(rx, ry)) return false;
  long x, y;
  if (!cal.swapped) {
    x = map(rx, cal.xLo, cal.xHi, 30, SCREEN_W - 30);
    y = map(ry, cal.yLo, cal.yHi, 30, SCREEN_H - 30);
  } else {
    x = map(ry, cal.xLo, cal.xHi, 30, SCREEN_W - 30);
    y = map(rx, cal.yLo, cal.yHi, 30, SCREEN_H - 30);
  }
  sx = constrain((int)x, 0, SCREEN_W - 1);
  sy = constrain((int)y, 0, SCREEN_H - 1);
  return true;
}

// ---------- Map ----------
static const int MAP_W = 16;
static const int MAP_H = 12;

// '1'/'2'/'3' = wall types (different shades), '.' = floor, 'P' = player start, 'E' = enemy spawn
const char *LEVELS[][MAP_H] = {
  {
    "1111111111111111",
    "1..............1",
    "1.111.11.1111..1",
    "1.1...........11",
    "1.1.11111.11.1.1",
    "1...1E......1..1",
    "1P..1..1111.1..1",
    "1...1..1..E.1..1",
    "1.11.1.1....1.11",
    "1....1.111111..1",
    "1..............1",
    "1111111111111111",
  },
  {
    "1111111111111111",
    "1P...1........31",
    "1.22.1.2222.2.31",
    "1.22...2..2.2..1",
    "1.2222.2E.2.222.",
    "1......2..2....1",
    "1.2222.222.2222.",
    "1.2..E.....2...1",
    "1.2.222222.2.2.1",
    "1...2....E.2...1",
    "1E..2......2..E1",
    "1111111111111111",
  },
  {
    "1111111111111111",
    "1P.............1",
    "1.3333333333333.",
    "1.3...........3.",
    "1.3.EE.....EE.3.",
    "1.3...........3.",
    "1.3...........3.",
    "1.3...........3.",
    "1.3.EE.....EE.3.",
    "1.3...........3.",
    "1.3333333333333.",
    "1111111111111111",
  },
};
static const int LEVEL_COUNT = 3;

char worldMap[MAP_H][MAP_W + 1];

inline bool isWallCell(int cx, int cy) {
  if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) return true;
  char c = worldMap[cy][cx];
  return c == '1' || c == '2' || c == '3';
}

uint16_t wallColor(char c, bool darkSide) {
  uint16_t base;
  switch (c) {
    case '2': base = tft.color565(60, 120, 220); break;
    case '3': base = tft.color565(200, 60, 60); break;
    default:  base = tft.color565(150, 150, 150); break;
  }
  if (darkSide) base = tft.color565(
    ((base >> 11) & 0x1F) * 8 * 0.6,
    ((base >> 5) & 0x3F) * 4 * 0.6,
    (base & 0x1F) * 8 * 0.6);
  return base;
}

// ---------- Game state ----------
struct Player {
  float x, y, angle;
  int health, ammo, score;
} player;

static const int MAX_ENEMIES = 16;
struct Enemy {
  float x, y;
  bool alive;
  int health;
  float hitFlash;
} enemies[MAX_ENEMIES];
int enemyCount = 0;

int curLevel = 0;
float zbuffer[SCREEN_W];
float rayOffset[SCREEN_W];
float cosOffset[SCREEN_W];

enum GameState { ST_TITLE, ST_PLAYING, ST_LEVEL_CLEAR, ST_WIN, ST_GAMEOVER };
GameState state = ST_TITLE;
float stateTimer = 0;

const float FOV = 1.047f; // 60 degrees
const float MAX_DEPTH = 20.0f;
const float MOVE_SPEED = 3.0f;
const float TURN_SPEED = 2.6f;
const float PLAYER_RADIUS = 0.25f;
const int PLAYER_MAX_HEALTH = 100;
const int AMMO_MAX = 20;

float fireCooldown = 0;
float ammoRegenTimer = 0;
float damageFlash = 0;
float muzzleFlash = 0;
float gunBob = 0;

void loadLevel(int lvl) {
  enemyCount = 0;
  for (int r = 0; r < MAP_H; r++) {
    for (int c = 0; c < MAP_W; c++) {
      char ch = LEVELS[lvl][r][c];
      if (ch == 'P') {
        player.x = c + 0.5f;
        player.y = r + 0.5f;
        worldMap[r][c] = '.';
      } else if (ch == 'E') {
        if (enemyCount < MAX_ENEMIES) {
          enemies[enemyCount].x = c + 0.5f;
          enemies[enemyCount].y = r + 0.5f;
          enemies[enemyCount].alive = true;
          enemies[enemyCount].health = 3;
          enemies[enemyCount].hitFlash = 0;
          enemyCount++;
        }
        worldMap[r][c] = '.';
      } else {
        worldMap[r][c] = ch;
      }
    }
    worldMap[r][MAP_W] = 0;
  }
  player.angle = 0;
}

void startGame() {
  player.health = PLAYER_MAX_HEALTH;
  player.ammo = AMMO_MAX;
  player.score = 0;
  curLevel = 0;
  loadLevel(curLevel);
  state = ST_PLAYING;
}

bool canMoveTo(float x, float y) {
  if (isWallCell((int)(x - PLAYER_RADIUS), (int)(y))) return false;
  if (isWallCell((int)(x + PLAYER_RADIUS), (int)(y))) return false;
  if (isWallCell((int)(x), (int)(y - PLAYER_RADIUS))) return false;
  if (isWallCell((int)(x), (int)(y + PLAYER_RADIUS))) return false;
  return true;
}

// DDA raycast. Returns distance, sets outChar/outDarkSide.
float castRay(float rayAngle, char &outChar, bool &outDark) {
  float dx = cosf(rayAngle), dy = sinf(rayAngle);
  int mapX = (int)player.x, mapY = (int)player.y;

  float deltaDistX = (dx == 0) ? 1e30f : fabsf(1.0f / dx);
  float deltaDistY = (dy == 0) ? 1e30f : fabsf(1.0f / dy);

  int stepX, stepY;
  float sideDistX, sideDistY;

  if (dx < 0) { stepX = -1; sideDistX = (player.x - mapX) * deltaDistX; }
  else { stepX = 1; sideDistX = (mapX + 1.0f - player.x) * deltaDistX; }
  if (dy < 0) { stepY = -1; sideDistY = (player.y - mapY) * deltaDistY; }
  else { stepY = 1; sideDistY = (mapY + 1.0f - player.y) * deltaDistY; }

  int side = 0;
  float dist = 0;
  for (int i = 0; i < 64; i++) {
    if (sideDistX < sideDistY) {
      sideDistX += deltaDistX; mapX += stepX; side = 0;
    } else {
      sideDistY += deltaDistY; mapY += stepY; side = 1;
    }
    if (isWallCell(mapX, mapY)) {
      outChar = (mapX < 0 || mapY < 0 || mapX >= MAP_W || mapY >= MAP_H) ? '1' : worldMap[mapY][mapX];
      outDark = (side == 1);
      dist = (side == 0) ? (sideDistX - deltaDistX) : (sideDistY - deltaDistY);
      return dist;
    }
    if (i == 63) { outChar = '1'; outDark = false; return MAX_DEPTH; }
  }
  return MAX_DEPTH;
}

void renderScene() {
  // Ceiling / floor as flat bands (fast, no texture sampling)
  fb.fillRect(0, 0, SCREEN_W, SCREEN_H / 2, fb.color565(30, 30, 40));
  fb.fillRect(0, SCREEN_H / 2, SCREEN_W, SCREEN_H / 2, fb.color565(50, 45, 35));

  for (int col = 0; col < SCREEN_W; col++) {
    float rayAngle = player.angle + rayOffset[col];
    char wc; bool dark;
    float dist = castRay(rayAngle, wc, dark);
    float perp = dist * cosOffset[col];
    if (perp < 0.05f) perp = 0.05f;
    zbuffer[col] = perp;

    int lineH = (int)(SCREEN_H / perp);
    int drawStart = SCREEN_H / 2 - lineH / 2;
    int drawEnd = SCREEN_H / 2 + lineH / 2;
    int cs = drawStart < 0 ? 0 : drawStart;
    int ce = drawEnd >= SCREEN_H ? SCREEN_H - 1 : drawEnd;
    if (ce >= cs) {
      uint16_t c = wallColor(wc, dark);
      // distance shading: fade toward background color at range
      float fade = constrain(1.0f - perp / MAX_DEPTH, 0.25f, 1.0f);
      uint8_t r = ((c >> 11) & 0x1F) * 8 * fade;
      uint8_t g = ((c >> 5) & 0x3F) * 4 * fade;
      uint8_t b = (c & 0x1F) * 8 * fade;
      fb.drawFastVLine(col, cs, ce - cs + 1, fb.color565(r, g, b));
    }
  }
}

void renderSprites() {
  // Painter's algorithm: draw farthest first.
  int order[MAX_ENEMIES];
  float dists[MAX_ENEMIES];
  int n = 0;
  for (int i = 0; i < enemyCount; i++) {
    if (!enemies[i].alive) continue;
    order[n] = i;
    float ddx = enemies[i].x - player.x, ddy = enemies[i].y - player.y;
    dists[n] = ddx * ddx + ddy * ddy;
    n++;
  }
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (dists[j] < dists[j + 1]) {
        float td = dists[j]; dists[j] = dists[j + 1]; dists[j + 1] = td;
        int ti = order[j]; order[j] = order[j + 1]; order[j + 1] = ti;
      }

  for (int k = 0; k < n; k++) {
    Enemy &e = enemies[order[k]];
    float dx = e.x - player.x, dy = e.y - player.y;
    float ca = cosf(-player.angle), sa = sinf(-player.angle);
    float tx = dx * ca - dy * sa;
    float ty = dx * sa + dy * ca;
    // ty: forward distance in camera space, tx: right offset
    if (ty < 0.2f) continue;

    float screenX = (SCREEN_W / 2.0f) * (1.0f + tx / (ty * tanf(FOV / 2)));
    float spriteH = SCREEN_H / ty;
    float spriteW = spriteH;
    int left = (int)(screenX - spriteW / 2);
    int right = (int)(screenX + spriteW / 2);
    int centerY = SCREEN_H / 2;

    bool flash = e.hitFlash > 0;
    uint16_t bodyColor = flash ? TFT_WHITE : fb.color565(140, 30, 20);
    uint16_t eyeColor = TFT_YELLOW;

    for (int col = left; col <= right; col++) {
      if (col < 0 || col >= SCREEN_W) continue;
      if (zbuffer[col] <= ty) continue;
      float u = (col - left) / (float)(right - left + 1);
      float rel = 2 * u - 1;
      float k2 = 1.0f - rel * rel;
      if (k2 < 0) continue;
      float halfH = (spriteH / 2.0f) * sqrtf(k2);
      int y0 = (int)(centerY - halfH);
      int y1 = (int)(centerY + halfH);
      if (y0 < 0) y0 = 0;
      if (y1 >= SCREEN_H) y1 = SCREEN_H - 1;
      if (y1 >= y0) fb.drawFastVLine(col, y0, y1 - y0 + 1, bodyColor);
    }
    // eyes
    if (!flash) {
      int eyeY = (int)(centerY - spriteH * 0.15f);
      int eL = (int)(screenX - spriteW * 0.18f);
      int eR = (int)(screenX + spriteW * 0.06f);
      int eyeSize = max(1, (int)(spriteW * 0.12f));
      if (eL >= 0 && eL < SCREEN_W && zbuffer[eL] > ty) fb.fillRect(eL, eyeY, eyeSize, eyeSize, eyeColor);
      if (eR >= 0 && eR < SCREEN_W && zbuffer[eR] > ty) fb.fillRect(eR, eyeY, eyeSize, eyeSize, eyeColor);
    }
  }
}

void renderGun() {
  int gx = SCREEN_W / 2;
  int gy = SCREEN_H - 10 + (int)(sinf(gunBob) * 4);
  fb.fillRect(gx - 45, gy - 55, 90, 60, fb.color565(70, 70, 75));
  fb.fillRect(gx - 12, gy - 95, 24, 45, fb.color565(50, 50, 55));
  if (muzzleFlash > 0) {
    fb.fillCircle(gx, gy - 100, 14, TFT_YELLOW);
    fb.fillCircle(gx, gy - 100, 7, TFT_WHITE);
  }
}

void renderHUD() {
  fb.setTextDatum(TL_DATUM);
  fb.fillRect(0, 0, 130, 14, TFT_BLACK);
  fb.setTextColor(TFT_GREEN, TFT_BLACK);
  fb.drawString("HP " + String(player.health), 4, 2, 2);
  fb.fillRect(SCREEN_W - 90, 0, 90, 14, TFT_BLACK);
  fb.setTextColor(TFT_YELLOW, TFT_BLACK);
  fb.setTextDatum(TR_DATUM);
  fb.drawString("AMMO " + String(player.ammo), SCREEN_W - 4, 2, 2);
  fb.setTextDatum(TL_DATUM);
  fb.fillRect(0, 16, 100, 14, TFT_BLACK);
  fb.setTextColor(TFT_CYAN, TFT_BLACK);
  fb.drawString("SCORE " + String(player.score), 4, 16, 2);

  if (damageFlash > 0) {
    uint8_t a = (uint8_t)(damageFlash * 120);
    // cheap flash: red border
    fb.drawRect(0, 0, SCREEN_W, SCREEN_H, TFT_RED);
    fb.drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, TFT_RED);
  }
}

// ---------- Touch control zones ----------
struct Btn { int x, y, r; };
Btn btnUp    = {55, 150, 22};
Btn btnDown  = {55, 210, 22};
Btn btnLeft  = {15, 190, 20};
Btn btnRight = {95, 190, 20};
Btn btnFire  = {SCREEN_W - 45, SCREEN_H - 45, 30};

bool inBtn(Btn &b, int x, int y) {
  int dx = x - b.x, dy = y - b.y;
  return dx * dx + dy * dy <= b.r * b.r;
}

void drawControlsOverlay() {
  uint16_t c = fb.color565(255, 255, 255);
  fb.drawCircle(btnUp.x, btnUp.y, btnUp.r, c);
  fb.drawCircle(btnDown.x, btnDown.y, btnDown.r, c);
  fb.drawCircle(btnLeft.x, btnLeft.y, btnLeft.r, c);
  fb.drawCircle(btnRight.x, btnRight.y, btnRight.r, c);
  fb.drawCircle(btnFire.x, btnFire.y, btnFire.r, TFT_RED);
  fb.setTextDatum(MC_DATUM);
  fb.setTextColor(TFT_WHITE);
  fb.drawString("^", btnUp.x, btnUp.y, 2);
  fb.drawString("v", btnDown.x, btnDown.y, 2);
  fb.drawString("<", btnLeft.x, btnLeft.y, 2);
  fb.drawString(">", btnRight.x, btnRight.y, 2);
  fb.setTextColor(TFT_RED);
  fb.drawString("FIRE", btnFire.x, btnFire.y, 2);
}

void fireWeapon() {
  if (player.ammo <= 0 || fireCooldown > 0) return;
  player.ammo--;
  fireCooldown = 0.35f;
  muzzleFlash = 0.12f;

  char wc; bool dark;
  float wallDist = castRay(player.angle, wc, dark);

  int bestIdx = -1;
  float bestDist = 1e9f;
  for (int i = 0; i < enemyCount; i++) {
    if (!enemies[i].alive) continue;
    float dx = enemies[i].x - player.x, dy = enemies[i].y - player.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist > wallDist) continue; // wall blocks shot
    float angToEnemy = atan2f(dy, dx);
    float diff = angToEnemy - player.angle;
    while (diff > PI) diff -= 2 * PI;
    while (diff < -PI) diff += 2 * PI;
    if (fabsf(diff) < 0.12f && dist < bestDist) {
      bestDist = dist;
      bestIdx = i;
    }
  }
  if (bestIdx >= 0) {
    enemies[bestIdx].health--;
    enemies[bestIdx].hitFlash = 0.15f;
    if (enemies[bestIdx].health <= 0) {
      enemies[bestIdx].alive = false;
      player.score += 100;
    }
  }
}

bool anyEnemiesAlive() {
  for (int i = 0; i < enemyCount; i++) if (enemies[i].alive) return true;
  return false;
}

void updateEnemies(float dt) {
  for (int i = 0; i < enemyCount; i++) {
    Enemy &e = enemies[i];
    if (!e.alive) continue;
    if (e.hitFlash > 0) e.hitFlash -= dt;
    float dx = player.x - e.x, dy = player.y - e.y;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 8.0f && dist > 0.65f) {
      float nx = dx / dist, ny = dy / dist;
      float tryx = e.x + nx * 0.8f * dt;
      float tryy = e.y + ny * 0.8f * dt;
      if (!isWallCell((int)tryx, (int)e.y)) e.x = tryx;
      if (!isWallCell((int)e.x, (int)tryy)) e.y = tryy;
    } else if (dist <= 0.65f) {
      player.health -= (int)(15 * dt);
      damageFlash = 0.2f;
    }
  }
}

void handleInput(float dt, bool touching, int tx, int ty) {
  if (touching) {
    if (inBtn(btnUp, tx, ty)) {
      float nx = player.x + cosf(player.angle) * MOVE_SPEED * dt;
      float ny = player.y + sinf(player.angle) * MOVE_SPEED * dt;
      if (canMoveTo(nx, player.y)) player.x = nx;
      if (canMoveTo(player.x, ny)) player.y = ny;
      gunBob += dt * 12;
    } else if (inBtn(btnDown, tx, ty)) {
      float nx = player.x - cosf(player.angle) * MOVE_SPEED * dt;
      float ny = player.y - sinf(player.angle) * MOVE_SPEED * dt;
      if (canMoveTo(nx, player.y)) player.x = nx;
      if (canMoveTo(player.x, ny)) player.y = ny;
      gunBob += dt * 12;
    }
    if (inBtn(btnLeft, tx, ty)) {
      player.angle -= TURN_SPEED * dt;
    } else if (inBtn(btnRight, tx, ty)) {
      player.angle += TURN_SPEED * dt;
    }
    if (inBtn(btnFire, tx, ty)) {
      fireWeapon();
    }
  }
}

void showCenteredMessage(const char *line1, const char *line2) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line1, SCREEN_W / 2, SCREEN_H / 2 - 12, 4);
  if (line2) tft.drawString(line2, SCREEN_W / 2, SCREEN_H / 2 + 16, 2);
}

unsigned long lastMicros;

void setup() {
  Serial.begin(115200);
  SPI.begin(14, 12, 13, 15); // display bus (VSPI): sclk, miso, mosi, cs
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  fb.setColorDepth(16);
  const int bandCandidates[] = {120, 80, 60, 40, 20};
  void *sprPtr = nullptr;
  for (int cand : bandCandidates) {
    sprPtr = fb.createSprite(SCREEN_W, cand);
    if (sprPtr) { gBandH = cand; break; }
  }
  Serial.printf("[boot] sprite ptr=%p bandH=%d heap=%u largest8bit=%u\n", sprPtr, gBandH,
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  touchBegin();

  prefs.begin("doomcal2", false); // bumped: calibration format changed (added axis-swap detection)

  // Give the user ~1.2s during the title flash to force recalibration.
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("Hold screen to calibrate touch...", SCREEN_W / 2, SCREEN_H / 2, 2);
  unsigned long t0 = millis();
  bool forceCal = false;
  while (millis() - t0 < 1200) {
    if (touchIsTouched()) { forceCal = true; break; }
    delay(20);
  }

  bool haveCal = prefs.getBool("valid", false);
  if (!haveCal || forceCal) {
    runCalibration();
  } else {
    cal.swapped = prefs.getBool("swapped");
    cal.xLo = prefs.getInt("xLo");
    cal.xHi = prefs.getInt("xHi");
    cal.yLo = prefs.getInt("yLo");
    cal.yHi = prefs.getInt("yHi");
    cal.valid = true;
  }

  for (int col = 0; col < SCREEN_W; col++) {
    rayOffset[col] = ((col / (float)SCREEN_W) - 0.5f) * FOV;
    cosOffset[col] = cosf(rayOffset[col]);
  }

  randomSeed(analogRead(34));
  state = ST_TITLE;
  stateTimer = 0;
  lastMicros = micros();
}

void renderTitle() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_RED);
  tft.drawString("D O O M L I T E", SCREEN_W / 2, 60, 4);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("ESP32 Raycaster Clone", SCREEN_W / 2, 95, 2);
  tft.drawString("Tap anywhere to start", SCREEN_W / 2, 140, 2);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("D-pad left, FIRE right", SCREEN_W / 2, 190, 2);
}

void loop() {
  unsigned long now = micros();
  float dt = (now - lastMicros) / 1000000.0f;
  if (dt > 0.1f) dt = 0.1f;
  lastMicros = now;

  int tx, ty;
  bool touching = getTouchScreen(tx, ty);

  if (fireCooldown > 0) fireCooldown -= dt;
  if (muzzleFlash > 0) muzzleFlash -= dt;
  if (damageFlash > 0) damageFlash -= dt;
  ammoRegenTimer += dt;
  if (ammoRegenTimer > 2.5f && player.ammo < AMMO_MAX) {
    ammoRegenTimer = 0;
    player.ammo++;
  }

  switch (state) {
    case ST_TITLE:
      renderTitle();
      if (touching) { startGame(); }
      break;

    case ST_PLAYING: {
      handleInput(dt, touching, tx, ty);
      updateEnemies(dt);
      if (player.health <= 0) {
        player.health = 0;
        state = ST_GAMEOVER;
        stateTimer = 0;
        break;
      }
      for (int by = 0; by < SCREEN_H; by += gBandH) {
        fb.setViewport(0, -by, SCREEN_W, SCREEN_H, true);
        renderScene();
        renderSprites();
        renderGun();
        drawControlsOverlay();
        renderHUD();
        fb.pushSprite(0, by);
      }
      if (!anyEnemiesAlive()) {
        state = ST_LEVEL_CLEAR;
        stateTimer = 0;
      }
      break;
    }

    case ST_LEVEL_CLEAR: {
      for (int by = 0; by < SCREEN_H; by += gBandH) {
        fb.setViewport(0, -by, SCREEN_W, SCREEN_H, true);
        renderScene();
        renderGun();
        fb.pushSprite(0, by);
      }
      showCenteredMessage("LEVEL CLEAR", "get ready...");
      stateTimer += dt;
      if (stateTimer > 2.0f) {
        curLevel++;
        if (curLevel >= LEVEL_COUNT) {
          state = ST_WIN;
          stateTimer = 0;
        } else {
          player.health = min(PLAYER_MAX_HEALTH, player.health + 30);
          loadLevel(curLevel);
          state = ST_PLAYING;
        }
      }
      break;
    }

    case ST_WIN: {
      tft.fillScreen(TFT_BLACK);
      showCenteredMessage("YOU WIN", ("Score: " + String(player.score)).c_str());
      tft.setTextDatum(MC_DATUM);
      tft.drawString("Tap to play again", SCREEN_W / 2, SCREEN_H / 2 + 40, 2);
      if (touching) { state = ST_TITLE; }
      break;
    }

    case ST_GAMEOVER: {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED);
      showCenteredMessage("GAME OVER", ("Score: " + String(player.score)).c_str());
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Tap to restart", SCREEN_W / 2, SCREEN_H / 2 + 40, 2);
      stateTimer += dt;
      if (touching && stateTimer > 0.5f) { state = ST_TITLE; }
      break;
    }
  }
}
