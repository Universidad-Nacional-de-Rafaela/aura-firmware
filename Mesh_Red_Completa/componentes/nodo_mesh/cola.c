#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "cola.h"

static const char *TAG = "COLA";

/* ================================================================== */
/*  Respaldo en flash: registro circular de registros fijos de 256 B  */
/* ================================================================== */
#define COLA_SUBTIPO        0x40
#define REGISTRO_BYTES      256
#define SECTOR_BYTES        4096
#define POR_SECTOR          (SECTOR_BYTES / REGISTRO_BYTES)
#define MAGICO              0x41555241u     /* "AURA" */
#define PENDIENTE           0xFF
#define CONFIRMADA          0x00
#define SIN_FLASH           UINT32_MAX

typedef struct __attribute__((packed)) {
    uint32_t magico;
    uint32_t numero;                         /* creciente: ordena lo recuperado */
    uint8_t  estado;                         /* PENDIENTE o CONFIRMADA (fuera del CRC) */
    uint8_t  reservado[3];
    cola_muestra_t m;
    uint32_t crc;                            /* de magico, numero y m */
} registro_t;

_Static_assert(sizeof(registro_t) <= REGISTRO_BYTES, "el registro no entra en 256 B");

static const esp_partition_t *part = NULL;
static uint32_t total = 0;                   /* registros en la partición */
static uint32_t escritura = 0;               /* índice donde va el próximo */
static uint32_t proximo_numero = 1;

/* ================================================================== */
/*  Cola en RAM                                                        */
/* ================================================================== */
typedef struct {
    cola_muestra_t m;
    uint32_t idx_flash;                      /* SIN_FLASH = todavía solo en RAM */
} entrada_t;

static entrada_t *buf = NULL;
static SemaphoreHandle_t lock = NULL;
static uint32_t capacidad = 0;
static uint32_t cabeza = 0;                  /* la más vieja */
static uint32_t cantidad = 0;
static uint32_t descartadas = 0;
static uint32_t en_flash = 0;

static entrada_t *entrada(uint32_t i)        /* i = 0 es la más vieja */
{
    return &buf[(cabeza + i) % capacidad];
}

static uint32_t crc_de(const registro_t *r)
{
    uint32_t c = esp_rom_crc32_le(0, (const uint8_t *)&r->magico, 8);
    return esp_rom_crc32_le(c, (const uint8_t *)&r->m, sizeof(r->m));
}

static bool leer(uint32_t i, registro_t *r)
{
    return esp_partition_read(part, (size_t)i * REGISTRO_BYTES, r, sizeof(*r)) == ESP_OK;
}

static bool valido(const registro_t *r)
{
    return r->magico == MAGICO && r->crc == crc_de(r);
}

static bool en_blanco(uint32_t i)
{
    uint32_t b[REGISTRO_BYTES / 4];
    if (esp_partition_read(part, (size_t)i * REGISTRO_BYTES, b, sizeof(b)) != ESP_OK) {
        return false;
    }
    for (int k = 0; k < REGISTRO_BYTES / 4; k++) {
        if (b[k] != 0xFFFFFFFFu) {
            return false;
        }
    }
    return true;
}

/* Marca un registro como confirmado: un solo byte de 0xFF a 0x00, sin borrar */
static void flash_confirmar(entrada_t *e)
{
    if (part == NULL || e->idx_flash == SIN_FLASH) {
        return;
    }
    uint8_t confirmada = CONFIRMADA;
    esp_partition_write(part, (size_t)e->idx_flash * REGISTRO_BYTES + offsetof(registro_t, estado), &confirmada, 1);
    e->idx_flash = SIN_FLASH;
    en_flash--;
}

/* Con lock tomado. Escribe la medición en un lugar ya borrado; al entrar a un sector lo borra. */
static bool flash_escribir(entrada_t *e)
{
    if (part == NULL) {
        return false;
    }
    if (escritura % POR_SECTOR == 0) {
        if (esp_partition_erase_range(part, (size_t)escritura * REGISTRO_BYTES, SECTOR_BYTES) != ESP_OK) {
            return false;
        }
        /* Las que estaban respaldadas en ese sector vuelven a "solo RAM" (se respaldan de nuevo) */
        for (uint32_t i = 0; i < cantidad; i++) {
            entrada_t *x = entrada(i);
            if (x->idx_flash != SIN_FLASH && x->idx_flash / POR_SECTOR == escritura / POR_SECTOR) {
                x->idx_flash = SIN_FLASH;
                en_flash--;
            }
        }
    }
    registro_t r;
    memset(&r, 0xFF, sizeof(r));
    r.magico = MAGICO;
    r.numero = proximo_numero;
    r.estado = PENDIENTE;
    r.m = e->m;
    r.m.otro_arranque = 0;
    r.crc = crc_de(&r);
    if (esp_partition_write(part, (size_t)escritura * REGISTRO_BYTES, &r, sizeof(r)) != ESP_OK) {
        return false;
    }
    e->idx_flash = escritura;
    en_flash++;
    proximo_numero++;
    escritura = (escritura + 1) % total;
    return true;
}

/* Recupera lo pendiente de la flash, en orden, al principio de la cola */
static void flash_recuperar(void)
{
    part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, COLA_SUBTIPO, "cola");
    if (part == NULL) {
        ESP_LOGW(TAG, "No hay partición \"cola\": las mediciones no sobreviven a un reinicio");
        return;
    }
    total = (uint32_t)(part->size / REGISTRO_BYTES);
    registro_t r;
    uint32_t max_num = 0, idx_max = 0;
    bool hay = false;
    /* Primero se ubican las pendientes (índice y número), de a lo que entre en la RAM */
    uint32_t *pend_idx = heap_caps_malloc(capacidad * sizeof(uint32_t) * 2, MALLOC_CAP_8BIT);
    uint32_t *pend_num = pend_idx ? pend_idx + capacidad : NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < total; i++) {
        if (!leer(i, &r) || !valido(&r)) {
            continue;
        }
        if (!hay || r.numero > max_num) {
            max_num = r.numero;
            idx_max = i;
            hay = true;
        }
        if (r.estado == PENDIENTE && pend_idx) {
            if (n < capacidad) {
                pend_idx[n] = i;
                pend_num[n] = r.numero;
                n++;
            } else {                         /* no entran: se queda con las más nuevas */
                uint32_t viejo = 0;
                for (uint32_t k = 1; k < n; k++) {
                    if (pend_num[k] < pend_num[viejo]) {
                        viejo = k;
                    }
                }
                if (r.numero > pend_num[viejo]) {
                    pend_idx[viejo] = i;
                    pend_num[viejo] = r.numero;
                }
            }
        }
    }
    if (hay) {
        proximo_numero = max_num + 1;
        escritura = (idx_max + 1) % total;
    }
    /* Si el lugar de escritura no está en blanco (corte a mitad de una escritura), se salta al
       sector siguiente, que se borra antes de usarse */
    if (escritura % POR_SECTOR != 0 && !en_blanco(escritura)) {
        escritura = (escritura / POR_SECTOR + 1) * POR_SECTOR % total;
    }
    /* Orden por número (inserción: son pocas) y a la RAM */
    for (uint32_t a = 1; a < n; a++) {
        for (uint32_t b = a; b > 0 && pend_num[b] < pend_num[b - 1]; b--) {
            uint32_t t = pend_num[b]; pend_num[b] = pend_num[b - 1]; pend_num[b - 1] = t;
            t = pend_idx[b]; pend_idx[b] = pend_idx[b - 1]; pend_idx[b - 1] = t;
        }
    }
    for (uint32_t k = 0; k < n; k++) {
        if (leer(pend_idx[k], &r) && valido(&r)) {
            entrada_t *e = entrada(cantidad++);
            e->m = r.m;
            e->m.otro_arranque = 1;          /* su uptime_s es del arranque anterior */
            e->idx_flash = pend_idx[k];
            en_flash++;
        }
    }
    heap_caps_free(pend_idx);
    ESP_LOGI(TAG, "Respaldo en flash: %lu lugares, %lu mediciones recuperadas", (unsigned long)total,
             (unsigned long)n);
}

/* ================================================================== */
/*  API                                                                */
/* ================================================================== */
esp_err_t cola_abrir(void)
{
    lock = xSemaphoreCreateMutex();
    /* Si no hay RAM para todas, se pide de a menos: mejor una cola chica que ninguna */
    for (capacidad = COLA_CAPACIDAD; capacidad >= 16; capacidad /= 2) {
        buf = heap_caps_malloc(capacidad * sizeof(entrada_t), MALLOC_CAP_8BIT);
        if (buf != NULL) {
            break;
        }
    }
    if (buf == NULL) {
        capacidad = 0;
        ESP_LOGE(TAG, "Sin RAM para la cola");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Cola en RAM: %lu mediciones (%lu KB); a flash si pasan %d s sin confirmar",
             (unsigned long)capacidad, (unsigned long)(capacidad * sizeof(entrada_t) / 1024), COLA_A_FLASH_S);
    flash_recuperar();
    return ESP_OK;
}

bool cola_guardar(const cola_muestra_t *m)
{
    if (buf == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (cantidad == capacidad) {             /* llena: se pierde la más vieja */
        flash_confirmar(entrada(0));          /* que no se recupere después */
        cabeza = (cabeza + 1) % capacidad;
        cantidad--;
        descartadas++;
    }
    entrada_t *e = entrada(cantidad);
    e->m = *m;
    e->m.otro_arranque = 0;
    e->idx_flash = SIN_FLASH;
    cantidad++;
    xSemaphoreGive(lock);
    return true;
}

bool cola_mirar(cola_muestra_t *m)
{
    if (buf == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = cantidad > 0;
    if (ok) {
        *m = entrada(0)->m;
    }
    xSemaphoreGive(lock);
    return ok;
}

bool cola_sacar(void)
{
    if (buf == NULL) {
        return false;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = cantidad > 0;
    if (ok) {
        flash_confirmar(entrada(0));
        cabeza = (cabeza + 1) % capacidad;
        cantidad--;
    }
    xSemaphoreGive(lock);
    return ok;
}

void cola_respaldar(uint32_t uptime_ahora, uint32_t epoca_ahora, bool todas)
{
    if (buf == NULL) {
        return;
    }
    uint32_t escritas = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    /* Las que se midieron sin hora, fechadas con el uptime apenas hay hora: así lo que pase a
       la flash ya lleva la hora en que se tomó, aunque la hoja se reinicie */
    for (uint32_t i = 0; epoca_ahora && i < cantidad; i++) {
        entrada_t *e = entrada(i);
        if (e->m.ts == 0 && !e->m.otro_arranque && e->idx_flash == SIN_FLASH) {
            e->m.ts = epoca_ahora - (uptime_ahora - e->m.uptime_s);
        }
    }
    for (uint32_t i = 0; part != NULL && i < cantidad; i++) {
        entrada_t *e = entrada(i);
        if (e->idx_flash != SIN_FLASH) {
            continue;                        /* ya respaldada (o recuperada) */
        }
        if (!todas && (e->m.otro_arranque || uptime_ahora - e->m.uptime_s < COLA_A_FLASH_S)) {
            break;                           /* las que siguen son más nuevas */
        }
        if (!flash_escribir(e)) {
            ESP_LOGW(TAG, "No se pudo respaldar en flash");
            break;
        }
        escritas++;
    }
    xSemaphoreGive(lock);
    if (escritas && todas) {
        ESP_LOGI(TAG, "%lu mediciones sin confirmar respaldadas en flash antes de reiniciar (en flash: %lu)",
                 (unsigned long)escritas, (unsigned long)en_flash);
    } else if (escritas) {
        ESP_LOGI(TAG, "%lu mediciones sin confirmar hace %d s: respaldadas en flash (en flash: %lu)",
                 (unsigned long)escritas, COLA_A_FLASH_S, (unsigned long)en_flash);
    }
}

uint32_t cola_cantidad(void)
{
    return cantidad;
}

uint32_t cola_descartadas(void)
{
    return descartadas;
}

uint32_t cola_en_flash(void)
{
    return en_flash;
}

uint32_t cola_capacidad(void)
{
    return capacidad;
}
