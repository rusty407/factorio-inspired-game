#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <vector>
#include <iostream>
#include <string>
#include <cmath>
#include <map>
#include <algorithm>
#include <cstdlib>
#include <ctime>

const int TILE_SIZE = 32;
const int GRID_W = 60;
const int GRID_H = 40;
const int WINDOW_W = 1280;
const int WINDOW_H = 720;

// ─── Tile types ──────────────────────────────────────────────────────────────
enum Tile {
    TILE_GRASS = 0,
    TILE_TREE,
    TILE_ROCK,
    TILE_WATER,
    TILE_PATH,
    TILE_CONVEYOR   // kept from original
};

// Returns true for tiles that block movement (ready for collision step)
inline bool isSolid(Tile t) {
    return t == TILE_TREE || t == TILE_ROCK || t == TILE_WATER;
}

// Tile render color
struct TileStyle { SDL_Color bg; SDL_Color border; };
TileStyle tileStyle(Tile t) {
    switch (t) {
        case TILE_GRASS:    return {{52,  101,  36, 255}, {44,  88,  28, 255}};
        case TILE_TREE:     return {{22,   65,  22, 255}, {14,  48,  14, 255}};
        case TILE_ROCK:     return {{110, 108, 100, 255}, {80,  78,  72, 255}};
        case TILE_WATER:    return {{30,   90, 160, 255}, {20,  70, 140, 255}};
        case TILE_PATH:     return {{160, 130,  80, 255}, {130, 105,  60, 255}};
        case TILE_CONVEYOR: return {{200, 180,  40, 200}, {160, 140,  30, 255}};
        default:            return {{52,  101,  36, 255}, {44,  88,  28, 255}};
    }
}

enum PlayerState { STATE_IDLE, STATE_WALK, STATE_RUN, STATE_ATTACK };

// ─── Inventory ───────────────────────────────────────────────────────────────
struct Item {
    std::string name;
    int count = 0;
};

const int INV_COLS = 10;
const int INV_ROWS = 5;
const int SLOT_SIZE = 64;
const int SLOT_PAD  = 6;

struct Inventory {
    Item slots[INV_COLS * INV_ROWS];
    bool open = false;

    Inventory() {
        slots[0] = {"Iron Plate",     47};
        slots[1] = {"Copper Plate",   23};
        slots[2] = {"Coal",          120};
        slots[3] = {"Stone",          35};
        slots[4] = {"Wood",           18};
        slots[5] = {"Iron Gear",      12};
        slots[6] = {"Copper Cable",   64};
        slots[7] = {"Green Circuit",   8};
        slots[8] = {"Iron Chest",      3};
        slots[9] = {"Conveyor Belt",  50};
    }
};

// ─── Animation ───────────────────────────────────────────────────────────────
struct Animation {
    SDL_Texture* texture  = nullptr;
    int frameCount  = 0;
    int frameWidth  = 0;
    int frameHeight = 0;
    float fps       = 8.0f;
};

SDL_Texture* loadTexture(SDL_Renderer* renderer, const std::string& path) {
    SDL_Surface* surf = IMG_Load(path.c_str());
    if (!surf) {
        std::cout << "Failed to load: " << path << " -> " << IMG_GetError() << "\n";
        return nullptr;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    return tex;
}

Animation loadAnim(SDL_Renderer* renderer, const std::string& path,
                   int frameCount, float fps) {
    Animation anim;
    anim.texture    = loadTexture(renderer, path);
    anim.frameCount = frameCount;
    anim.fps        = fps;
    if (anim.texture) {
        int w, h;
        SDL_QueryTexture(anim.texture, NULL, NULL, &w, &h);
        anim.frameWidth  = w / frameCount;
        anim.frameHeight = h;
        std::cout << "Loaded: " << path
                  << " | sheet=" << w << "x" << h
                  << " | frames=" << frameCount
                  << " | frameSize=" << anim.frameWidth << "x" << anim.frameHeight << "\n";
    }
    return anim;
}

// ─── Procedural map generation ───────────────────────────────────────────────

// Simple value noise — deterministic for a given seed position
float valueNoise(int x, int y, int seed) {
    int n = x + y * 57 + seed * 131;
    n = (n << 13) ^ n;
    return 1.0f - ((n * (n * n * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f;
}

// Smooth bilinear interpolation over value noise
float smoothNoise(float x, float y, int seed) {
    int ix = (int)x, iy = (int)y;
    float fx = x - ix, fy = y - iy;
    float s = fx * fx * (3 - 2 * fx);
    float t = fy * fy * (3 - 2 * fy);
    float n00 = valueNoise(ix,   iy,   seed);
    float n10 = valueNoise(ix+1, iy,   seed);
    float n01 = valueNoise(ix,   iy+1, seed);
    float n11 = valueNoise(ix+1, iy+1, seed);
    float nx0 = n00 + s * (n10 - n00);
    float nx1 = n01 + s * (n11 - n01);
    return nx0 + t * (nx1 - nx0);
}

// Fractional Brownian Motion — several octaves of noise layered
float fbm(float x, float y, int seed, int octaves = 4) {
    float value = 0, amplitude = 0.5f, frequency = 1.0f, max = 0;
    for (int i = 0; i < octaves; i++) {
        value     += smoothNoise(x * frequency, y * frequency, seed + i * 17) * amplitude;
        max       += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return value / max;
}

// Carve winding dirt paths using a random walk
void carvePaths(std::vector<Tile>& grid, int seed) {
    srand(seed ^ 0xDEAD);
    int numPaths = 3 + rand() % 3;
    for (int p = 0; p < numPaths; p++) {
        // Start from a random edge
        int x, y, dx = 0, dy = 0;
        int edge = rand() % 4;
        if (edge == 0) { x = rand() % GRID_W; y = 0;          dx = 0;  dy = 1; }
        else if (edge == 1) { x = rand() % GRID_W; y = GRID_H-1; dx = 0;  dy = -1;}
        else if (edge == 2) { x = 0;          y = rand() % GRID_H; dx = 1;  dy = 0; }
        else                { x = GRID_W-1;   y = rand() % GRID_H; dx = -1; dy = 0; }

        int steps = GRID_W + GRID_H;
        for (int s = 0; s < steps; s++) {
            for (int py = -1; py <= 1; py++) {
                for (int px = -1; px <= 1; px++) {
                    int cx = x + px, cy = y + py;
                    if (cx >= 0 && cx < GRID_W && cy >= 0 && cy < GRID_H) {
                        Tile& t = grid[cy * GRID_W + cx];
                        if (t != TILE_WATER) t = TILE_PATH;
                    }
                }
            }
            // Random walk with bias toward center
            int r = rand() % 10;
            if (r < 3)      { dx = (rand() % 3) - 1; dy = 0; }
            else if (r < 6) { dx = 0; dy = (rand() % 3) - 1; }
            if (dx == 0 && dy == 0) dx = 1;
            x += dx; y += dy;
            x = std::max(0, std::min(x, GRID_W-1));
            y = std::max(0, std::min(y, GRID_H-1));
        }
    }
}

// Generate a full Zelda-style overworld map
std::vector<Tile> generateMap(int seed) {
    std::vector<Tile> grid(GRID_W * GRID_H, TILE_GRASS);
    float scaleWater = 0.18f;
    float scaleTree  = 0.22f;
    float scaleRock  = 0.20f;

    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            float wx = x * scaleWater, wy = y * scaleWater;
            float tx = x * scaleTree,  ty = y * scaleTree;
            float rx = x * scaleRock,  ry = y * scaleRock;

            float waterVal = fbm(wx, wy, seed,          4);
            float treeVal  = fbm(tx, ty, seed + 1000,   3);
            float rockVal  = fbm(rx, ry, seed + 2000,   3);

            // Force water border around the edges (island feel)
            float edgeDist = std::min({(float)x, (float)y,
                                       (float)(GRID_W-1-x),
                                       (float)(GRID_H-1-y)}) / 4.0f;
            float edgeFactor = std::min(1.0f, edgeDist);
            waterVal = waterVal * edgeFactor + (1.0f - edgeFactor) * 0.8f;

            Tile t = TILE_GRASS;
            if      (waterVal > 0.6f)  t = TILE_WATER;
            else if (treeVal  > 0.55f) t = TILE_TREE;
            else if (rockVal  > 0.58f) t = TILE_ROCK;

            grid[y * GRID_W + x] = t;
        }
    }

    // Carve paths on top
    carvePaths(grid, seed);

    // Clear a small safe spawn area
    int spawnX = 5, spawnY = 5;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int gx = spawnX + dx, gy = spawnY + dy;
            if (gx >= 0 && gx < GRID_W && gy >= 0 && gy < GRID_H)
                if (isSolid(grid[gy * GRID_W + gx]))
                    grid[gy * GRID_W + gx] = TILE_GRASS;
        }

    return grid;
}

// ─── Draw rounded rect helper ────────────────────────────────────────────────
void drawRoundRect(SDL_Renderer* r, SDL_Rect rect, int radius,
                   SDL_Color fill, SDL_Color border) {
    SDL_SetRenderDrawColor(r, fill.r, fill.g, fill.b, fill.a);
    SDL_Rect inner = {rect.x + radius, rect.y, rect.w - 2*radius, rect.h};
    SDL_RenderFillRect(r, &inner);
    SDL_Rect left  = {rect.x, rect.y + radius, radius, rect.h - 2*radius};
    SDL_Rect right = {rect.x + rect.w - radius, rect.y + radius, radius, rect.h - 2*radius};
    SDL_RenderFillRect(r, &left);
    SDL_RenderFillRect(r, &right);
    SDL_Rect tl = {rect.x, rect.y, radius, radius};
    SDL_Rect tr = {rect.x + rect.w - radius, rect.y, radius, radius};
    SDL_Rect bl = {rect.x, rect.y + rect.h - radius, radius, radius};
    SDL_Rect br = {rect.x + rect.w - radius, rect.y + rect.h - radius, radius, radius};
    SDL_RenderFillRect(r, &tl); SDL_RenderFillRect(r, &tr);
    SDL_RenderFillRect(r, &bl); SDL_RenderFillRect(r, &br);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    SDL_RenderDrawRect(r, &rect);
}

// ─── Render inventory panel ──────────────────────────────────────────────────
void renderInventory(SDL_Renderer* renderer, TTF_Font* font,
                     TTF_Font* fontSmall, const Inventory& inv) {
    if (!inv.open) return;

    int panelW = INV_COLS * (SLOT_SIZE + SLOT_PAD) + SLOT_PAD + 20;
    int panelH = INV_ROWS * (SLOT_SIZE + SLOT_PAD) + SLOT_PAD + 50;
    int panelX = (WINDOW_W - panelW) / 2;
    int panelY = (WINDOW_H - panelH) / 2;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 20, 20, 28, 230);
    SDL_Rect panelRect = {panelX, panelY, panelW, panelH};
    SDL_RenderFillRect(renderer, &panelRect);

    SDL_SetRenderDrawColor(renderer, 200, 130, 40, 255);
    SDL_RenderDrawRect(renderer, &panelRect);

    SDL_SetRenderDrawColor(renderer, 35, 28, 15, 255);
    SDL_Rect titleBar = {panelX, panelY, panelW, 36};
    SDL_RenderFillRect(renderer, &titleBar);
    SDL_SetRenderDrawColor(renderer, 200, 130, 40, 255);
    SDL_RenderDrawRect(renderer, &titleBar);

    if (font) {
        SDL_Color white = {230, 180, 80, 255};
        SDL_Surface* ts = TTF_RenderText_Blended(font, "Inventory", white);
        if (ts) {
            SDL_Texture* tt = SDL_CreateTextureFromSurface(renderer, ts);
            SDL_Rect tdst = {panelX + 10, panelY + 6, ts->w, ts->h};
            SDL_RenderCopy(renderer, tt, NULL, &tdst);
            SDL_DestroyTexture(tt);
            SDL_FreeSurface(ts);
        }
    }

    if (fontSmall) {
        SDL_Color hint = {120, 100, 60, 200};
        SDL_Surface* hs = TTF_RenderText_Blended(fontSmall, "[E] Close", hint);
        if (hs) {
            SDL_Texture* ht = SDL_CreateTextureFromSurface(renderer, hs);
            SDL_Rect hdst = {panelX + panelW - hs->w - 10, panelY + 10, hs->w, hs->h};
            SDL_RenderCopy(renderer, ht, NULL, &hdst);
            SDL_DestroyTexture(ht);
            SDL_FreeSurface(hs);
        }
    }

    for (int row = 0; row < INV_ROWS; row++) {
        for (int col = 0; col < INV_COLS; col++) {
            int idx = row * INV_COLS + col;
            int sx = panelX + 10 + col * (SLOT_SIZE + SLOT_PAD);
            int sy = panelY + 44 + row * (SLOT_SIZE + SLOT_PAD);
            const Item& item = inv.slots[idx];
            bool hasItem = item.count > 0 && !item.name.empty();

            SDL_SetRenderDrawColor(renderer, hasItem ? 40 : 28, hasItem ? 38 : 28,
                                   hasItem ? 30 : 22, 255);
            SDL_Rect slotRect = {sx, sy, SLOT_SIZE, SLOT_SIZE};
            SDL_RenderFillRect(renderer, &slotRect);
            SDL_SetRenderDrawColor(renderer, hasItem ? 130 : 60,
                                   hasItem ? 100 : 55,
                                   hasItem ? 40  : 45, 255);
            SDL_RenderDrawRect(renderer, &slotRect);

            if (hasItem && fontSmall) {
                std::string abbr = item.name;
                if (abbr.size() > 8) abbr = abbr.substr(0, 7) + ".";
                SDL_Color nameCol = {200, 180, 120, 255};
                SDL_Surface* ns = TTF_RenderText_Blended(fontSmall, abbr.c_str(), nameCol);
                if (ns) {
                    int nw = ns->w, nh = ns->h;
                    if (nw > SLOT_SIZE - 4) nw = SLOT_SIZE - 4;
                    SDL_Texture* nt = SDL_CreateTextureFromSurface(renderer, ns);
                    SDL_Rect ndst = {sx + (SLOT_SIZE - nw) / 2, sy + 6, nw, nh};
                    SDL_RenderCopy(renderer, nt, NULL, &ndst);
                    SDL_DestroyTexture(nt);
                    SDL_FreeSurface(ns);
                }
                std::string cnt = std::to_string(item.count);
                SDL_Color cntCol = {255, 220, 60, 255};
                SDL_Surface* cs = TTF_RenderText_Blended(fontSmall, cnt.c_str(), cntCol);
                if (cs) {
                    SDL_Texture* ct = SDL_CreateTextureFromSurface(renderer, cs);
                    SDL_Rect cdst = {sx + SLOT_SIZE - cs->w - 4,
                                     sy + SLOT_SIZE - cs->h - 4, cs->w, cs->h};
                    SDL_RenderCopy(renderer, ct, NULL, &cdst);
                    SDL_DestroyTexture(ct);
                    SDL_FreeSurface(cs);
                }
            }
        }
    }
}

// ─── Render a tile with a simple decorative detail ───────────────────────────
void renderTile(SDL_Renderer* renderer, Tile t, int sx, int sy, int size) {
    TileStyle style = tileStyle(t);
    SDL_Rect r = {sx, sy, size, size};

    SDL_SetRenderDrawColor(renderer, style.bg.r, style.bg.g, style.bg.b, style.bg.a);
    SDL_RenderFillRect(renderer, &r);

    // Per-tile decoration (purely cosmetic, colored rectangles)
    int half = size / 2, qtr = size / 4;
    switch (t) {
        case TILE_TREE: {
            // Dark canopy circle approximation: two overlapping rects
            SDL_SetRenderDrawColor(renderer, 14, 52, 14, 255);
            SDL_Rect canopy1 = {sx + qtr,       sy + 2,        half,      half + 2};
            SDL_Rect canopy2 = {sx + qtr - 2,   sy + qtr,      half + 4,  half};
            SDL_RenderFillRect(renderer, &canopy1);
            SDL_RenderFillRect(renderer, &canopy2);
            // Trunk
            SDL_SetRenderDrawColor(renderer, 80, 50, 20, 255);
            SDL_Rect trunk = {sx + half - 2, sy + size - qtr, 4, qtr};
            SDL_RenderFillRect(renderer, &trunk);
            break;
        }
        case TILE_ROCK: {
            // Grey boulder shape
            SDL_SetRenderDrawColor(renderer, 140, 138, 128, 255);
            SDL_Rect stone = {sx + qtr, sy + qtr + 2, half + 4, half - 2};
            SDL_RenderFillRect(renderer, &stone);
            SDL_SetRenderDrawColor(renderer, 170, 168, 158, 255);
            SDL_Rect highlight = {sx + qtr + 2, sy + qtr + 4, qtr, qtr - 2};
            SDL_RenderFillRect(renderer, &highlight);
            break;
        }
        case TILE_WATER: {
            // Lighter wave stripe
            SDL_SetRenderDrawColor(renderer, 60, 130, 200, 120);
            SDL_Rect wave1 = {sx + 4,        sy + qtr,      size - 10, 4};
            SDL_Rect wave2 = {sx + 8,        sy + half + 2, size - 14, 4};
            SDL_RenderFillRect(renderer, &wave1);
            SDL_RenderFillRect(renderer, &wave2);
            break;
        }
        case TILE_PATH: {
            // Slightly lighter center strip
            SDL_SetRenderDrawColor(renderer, 180, 150, 100, 160);
            SDL_Rect strip = {sx + qtr - 2, sy + 2, half + 4, size - 4};
            SDL_RenderFillRect(renderer, &strip);
            break;
        }
        case TILE_CONVEYOR: {
            SDL_SetRenderDrawColor(renderer, 230, 210, 60, 255);
            for (int i = 0; i < 3; i++) {
                SDL_Rect arrow = {sx + 4, sy + 4 + i * (size / 3), size - 8, size / 3 - 2};
                SDL_RenderFillRect(renderer, &arrow);
            }
            break;
        }
        default: {
            // Grass: subtle variation dots
            SDL_SetRenderDrawColor(renderer, 60, 115, 44, 80);
            SDL_Rect dot1 = {sx + qtr,       sy + qtr,       3, 3};
            SDL_Rect dot2 = {sx + half + 4,  sy + half,      3, 3};
            SDL_Rect dot3 = {sx + qtr + 2,   sy + half + 4,  2, 2};
            SDL_RenderFillRect(renderer, &dot1);
            SDL_RenderFillRect(renderer, &dot2);
            SDL_RenderFillRect(renderer, &dot3);
            break;
        }
    }

    // Grid border
    SDL_SetRenderDrawColor(renderer, style.border.r, style.border.g,
                           style.border.b, 80);
    SDL_RenderDrawRect(renderer, &r);
}

// ─── Main ────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cout << "SDL init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
        std::cout << "IMG init failed: " << IMG_GetError() << "\n";
        return 1;
    }
    if (TTF_Init() != 0) {
        std::cout << "TTF init failed: " << TTF_GetError() << "\n";
    }

    SDL_Window* window = SDL_CreateWindow(
        "Mini Factorio", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_W, WINDOW_H, 0);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // ── Fonts ──────────────────────────────────────────────────────────────
    TTF_Font* font      = nullptr;
    TTF_Font* fontSmall = nullptr;
    const char* fontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        nullptr
    };
    for (int i = 0; fontPaths[i]; i++) {
        font      = TTF_OpenFont(fontPaths[i], 18);
        fontSmall = TTF_OpenFont(fontPaths[i], 12);
        if (font && fontSmall) break;
        if (font)      { TTF_CloseFont(font);      font      = nullptr; }
        if (fontSmall) { TTF_CloseFont(fontSmall); fontSmall = nullptr; }
    }

    // ── Sprites ────────────────────────────────────────────────────────────
    std::string spritePath = "/home/console/pix/knight-sprites/Sprites/without_outline/";
    Animation animIdle   = loadAnim(renderer, spritePath + "IDLE.png",   4,  6.0f);
    Animation animWalk   = loadAnim(renderer, spritePath + "WALK.png",   8, 10.0f);
    Animation animRun    = loadAnim(renderer, spritePath + "RUN.png",    8, 12.0f);
    Animation animAttack = loadAnim(renderer, spritePath + "ATTACK 1.png", 6, 14.0f);
    if (animIdle.texture) { animIdle.frameWidth = 96; animIdle.frameHeight = 84; }

    // ── Generate map ───────────────────────────────────────────────────────
    int mapSeed = (int)time(nullptr);
    std::vector<Tile> grid = generateMap(mapSeed);
    std::cout << "Map generated with seed: " << mapSeed << "\n";

    // ── Game state ─────────────────────────────────────────────────────────
    float playerX = 5 * TILE_SIZE;
    float playerY = 5 * TILE_SIZE;
    float camX = 0, camY = 0;
    float zoom = 1.0f;

    const float ZOOM_MIN  = 0.3f;
    const float ZOOM_MAX  = 3.0f;
    const float ZOOM_STEP = 0.1f;

    bool facingLeft  = false;
    PlayerState playerState = STATE_IDLE;
    float animTime = 0.0f;

    bool attacking     = false;
    bool attackQueued  = false;
    bool qWasUp        = true;

    Inventory inventory;
    bool eWasUp = true;

    // R to regenerate map
    bool rWasUp = true;

    Uint32 lastTick = SDL_GetTicks();
    bool running = true;

    while (running) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - lastTick) / 1000.0f;
        lastTick = now;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;

            if (ev.type == SDL_MOUSEWHEEL) {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                float worldMouseX = mx / zoom + camX;
                float worldMouseY = my / zoom + camY;
                if (ev.wheel.y > 0) zoom = std::min(zoom + ZOOM_STEP, ZOOM_MAX);
                else if (ev.wheel.y < 0) zoom = std::max(zoom - ZOOM_STEP, ZOOM_MIN);
                camX = worldMouseX - mx / zoom;
                camY = worldMouseY - my / zoom;
            }

            // Place conveyor on left-click (only when inventory closed)
            if (ev.type == SDL_MOUSEBUTTONDOWN && !inventory.open) {
                float worldX = (ev.button.x / zoom + camX) / TILE_SIZE;
                float worldY = (ev.button.y / zoom + camY) / TILE_SIZE;
                int tx = (int)worldX, ty = (int)worldY;
                if (tx >= 0 && tx < GRID_W && ty >= 0 && ty < GRID_H)
                    grid[ty * GRID_W + tx] = TILE_CONVEYOR;
            }
        }

        const Uint8* keys = SDL_GetKeyboardState(NULL);

        // ── Regen map (R) ──────────────────────────────────────────────────
        bool rDown = keys[SDL_SCANCODE_R];
        if (rDown && rWasUp && !inventory.open) {
            mapSeed = rand();
            grid = generateMap(mapSeed);
            playerX = 5 * TILE_SIZE;
            playerY = 5 * TILE_SIZE;
            std::cout << "Map regenerated with seed: " << mapSeed << "\n";
        }
        rWasUp = !rDown;

        // ── Inventory toggle (E) ────────────────────────────────────────────
        bool eDown = keys[SDL_SCANCODE_E];
        if (eDown && eWasUp) inventory.open = !inventory.open;
        eWasUp = !eDown;

        // ── Attack trigger (Q) ─────────────────────────────────────────────
        bool qDown = keys[SDL_SCANCODE_Q];
        if (qDown && qWasUp) {
            if (!attacking) { attacking = true; animTime = 0.0f; playerState = STATE_ATTACK; }
            else attackQueued = true;
        }
        qWasUp = !qDown;

        // ── Movement ───────────────────────────────────────────────────────
        bool shift  = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
        bool moving = false;

        if (!inventory.open && !attacking) {
            float speed = shift ? 4.5f : 2.5f;
            float nextX = playerX, nextY = playerY;

            moving = keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_S] ||
                     keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_D];

            if (keys[SDL_SCANCODE_A]) { nextX -= speed; facingLeft = true;  }
            if (keys[SDL_SCANCODE_D]) { nextX += speed; facingLeft = false; }
            if (keys[SDL_SCANCODE_W]) nextY -= speed;
            if (keys[SDL_SCANCODE_S]) nextY += speed;

            // ── COLLISION (tile-based — enabled now!) ──────────────────────
            // Check the tile the player's feet would land on
            auto tileAt = [&](float wx, float wy) -> Tile {
                int tx = (int)(wx / TILE_SIZE);
                int ty = (int)(wy / TILE_SIZE);
                if (tx < 0 || tx >= GRID_W || ty < 0 || ty >= GRID_H) return TILE_ROCK;
                return grid[ty * GRID_W + tx];
            };

            // Try horizontal
            if (!isSolid(tileAt(nextX, playerY))) playerX = nextX;
            // Try vertical
            if (!isSolid(tileAt(playerX, nextY))) playerY = nextY;
        }

        playerX = std::max(0.0f, std::min(playerX, (GRID_W - 1) * TILE_SIZE * 1.0f));
        playerY = std::max(0.0f, std::min(playerY, (GRID_H - 1) * TILE_SIZE * 1.0f));

        // ── Select animation ────────────────────────────────────────────────
        Animation* anim = &animIdle;

        if (attacking) {
            anim = animAttack.texture ? &animAttack : &animIdle;
            animTime += dt;
            float animLength = anim->frameCount / anim->fps;
            if (animTime >= animLength) {
                if (attackQueued) { attackQueued = false; animTime = 0.0f; }
                else { attacking = false; animTime = 0.0f; playerState = STATE_IDLE; }
            }
        } else {
            PlayerState newState = STATE_IDLE;
            if (moving && shift)  newState = STATE_RUN;
            else if (moving)      newState = STATE_WALK;
            if (newState != playerState) { playerState = newState; animTime = 0.0f; }
            if (playerState == STATE_WALK && animWalk.texture) anim = &animWalk;
            if (playerState == STATE_RUN  && animRun.texture)  anim = &animRun;
            animTime += dt;
        }

        int frame = (int)(animTime * anim->fps) % anim->frameCount;

        // ── Camera ─────────────────────────────────────────────────────────
        camX = playerX - (WINDOW_W / zoom) / 2.0f + TILE_SIZE / 2.0f;
        camY = playerY - (WINDOW_H / zoom) / 2.0f + TILE_SIZE / 2.0f;
        float maxCamX = GRID_W * TILE_SIZE - WINDOW_W / zoom;
        float maxCamY = GRID_H * TILE_SIZE - WINDOW_H / zoom;
        camX = std::max(0.0f, std::min(camX, maxCamX));
        camY = std::max(0.0f, std::min(camY, maxCamY));

        // ── RENDER ─────────────────────────────────────────────────────────
        SDL_RenderClear(renderer);

        auto worldToScreen = [&](float wx, float wy, int& sx, int& sy) {
            sx = (int)((wx - camX) * zoom);
            sy = (int)((wy - camY) * zoom);
        };

        int scaledTile = (int)(TILE_SIZE * zoom);

        // Render all tiles
        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                int sx, sy;
                worldToScreen(x * TILE_SIZE, y * TILE_SIZE, sx, sy);
                if (sx + scaledTile < 0 || sx > WINDOW_W ||
                    sy + scaledTile < 0 || sy > WINDOW_H) continue;
                renderTile(renderer, grid[y * GRID_W + x], sx, sy, scaledTile);
            }
        }

        // Player sprite
        if (anim->texture) {
            SDL_Rect src = {frame * anim->frameWidth, 0, anim->frameWidth, anim->frameHeight};
            const float spriteScale = 2.0f;
            int drawW = (int)(anim->frameWidth  * spriteScale * zoom);
            int drawH = (int)(anim->frameHeight * spriteScale * zoom);
            int sx, sy;
            worldToScreen(playerX, playerY, sx, sy);
            int offsetX = (int)(anim->frameWidth * 0.25f * spriteScale * zoom);
            SDL_Rect dst = {sx - offsetX, sy - drawH / 2 + 6, drawW, drawH};
            SDL_RendererFlip flip = facingLeft ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
            SDL_RenderCopyEx(renderer, anim->texture, &src, &dst, 0, NULL, flip);
        } else {
            // Fallback: draw a simple colored square for the player
            int sx, sy;
            worldToScreen(playerX, playerY, sx, sy);
            int ps = (int)(24 * zoom);
            SDL_SetRenderDrawColor(renderer, 220, 80, 80, 255);
            SDL_Rect pr = {sx - ps/2, sy - ps/2, ps, ps};
            SDL_RenderFillRect(renderer, &pr);
        }

        // ── HUD: keybind hints (bottom-left) ───────────────────────────────
        if (fontSmall) {
            const char* hints[] = {
                "[WASD] Move", "[Shift] Run",
                "[Q] Attack",  "[E] Inventory",
                "[LMB] Place Conveyor", "[R] New Map"
            };
            SDL_Color hc = {200, 200, 180, 180};
            int hy = WINDOW_H - 10;
            for (int i = 5; i >= 0; i--) {
                SDL_Surface* hs = TTF_RenderText_Blended(fontSmall, hints[i], hc);
                if (hs) {
                    SDL_Texture* ht = SDL_CreateTextureFromSurface(renderer, hs);
                    SDL_Rect hd = {10, hy - hs->h, hs->w, hs->h};
                    SDL_RenderCopy(renderer, ht, NULL, &hd);
                    SDL_DestroyTexture(ht);
                    SDL_FreeSurface(hs);
                    hy -= hs->h + 2;
                }
            }
        }

        // ── Inventory panel ────────────────────────────────────────────────
        renderInventory(renderer, font, fontSmall, inventory);

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    // Cleanup
    if (animIdle.texture)   SDL_DestroyTexture(animIdle.texture);
    if (animWalk.texture)   SDL_DestroyTexture(animWalk.texture);
    if (animRun.texture)    SDL_DestroyTexture(animRun.texture);
    if (animAttack.texture) SDL_DestroyTexture(animAttack.texture);
    if (font)               TTF_CloseFont(font);
    if (fontSmall)          TTF_CloseFont(fontSmall);

    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
