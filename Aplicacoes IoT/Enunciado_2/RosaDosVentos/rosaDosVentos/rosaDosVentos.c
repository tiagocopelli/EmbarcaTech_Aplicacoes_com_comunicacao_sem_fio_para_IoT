#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

// ==== CONFIGURAÇÕES ====
#define WIFI_SSID "copelli4" //Nome da rede
#define WIFI_PASSWORD "copelli4" //Senha da rede
#define NOTEBOOK_IP "192.168.0.228" // IP do notebook
#define UDP_PORT 8081

// ==== LEDS ====
#define LED_WIFI_OK 11
#define LED_WIFI_ERR 12
#define LED_STATUS 13

// ==== JOYSTICK ====
#define JOY_VRX 27  // ADC1
#define JOY_VRY 26  // ADC0
#define JOY_SW  22  // Digital (puxado para cima)

// ==== VARIÁVEIS ====
struct udp_pcb *udp_conn;
ip_addr_t notebook_addr;

void init_leds() {
    gpio_init(LED_WIFI_OK);
    gpio_init(LED_WIFI_ERR);
    gpio_init(LED_STATUS);
    gpio_set_dir(LED_WIFI_OK, GPIO_OUT);
    gpio_set_dir(LED_WIFI_ERR, GPIO_OUT);
    gpio_set_dir(LED_STATUS, GPIO_OUT);
    gpio_put(LED_WIFI_OK, 0);
    gpio_put(LED_WIFI_ERR, 0);
    gpio_put(LED_STATUS, 0);
}

void init_joystick() {
    adc_init();
    adc_gpio_init(JOY_VRX);
    adc_gpio_init(JOY_VRY);
    gpio_init(JOY_SW);
    gpio_set_dir(JOY_SW, GPIO_IN);
    gpio_pull_up(JOY_SW);

}

uint16_t read_adc(uint channel) {
    adc_select_input(channel);
    return adc_read();
}

bool connect_wifi() {
    printf("Conectando ao Wi-Fi...\n");
    if (cyw43_arch_init()) {
        printf("Erro ao inicializar Wi-Fi\n");
        gpio_put(LED_WIFI_ERR, 1);
        return false;
    }

    cyw43_arch_enable_sta_mode();

    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 20000)) {
        printf("Falha na conexão Wi-Fi\n");
        gpio_put(LED_WIFI_ERR, 1);
        return false;
    }

    printf("Conectado! IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    gpio_put(LED_WIFI_OK, 1);
    return true;
}

bool setup_udp() {
    if (!ipaddr_aton(NOTEBOOK_IP, &notebook_addr)) {
        printf("IP do notebook inválido\n");
        gpio_put(LED_WIFI_ERR, 1);
        return false;
    }

    udp_conn = udp_new();
    if (!udp_conn) {
        printf("Erro ao criar socket UDP\n");
        gpio_put(LED_WIFI_ERR, 1);
        return false;
    }

    printf("Configurado para enviar para %s:%d\n", ip4addr_ntoa(&notebook_addr), UDP_PORT);
    return true;
}

void send_udp_message(const char *message) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, strlen(message), PBUF_RAM);
    if (!p) {
        printf("Erro alocando buffer\n");
        return;
    }

    memcpy(p->payload, message, strlen(message));
    err_t err = udp_sendto(udp_conn, p, &notebook_addr, UDP_PORT);
    pbuf_free(p);

    if (err != ERR_OK) {
        printf("Erro enviando mensagem: %d\n", err);
        gpio_put(LED_STATUS, 0);
    } else {
        printf("Mensagem enviada: %s\n", message);
        gpio_put(LED_STATUS, 1);
    }
}

int main() {
    stdio_init_all();
    init_leds();
    init_joystick();

    if (!connect_wifi() || !setup_udp()) {
        while (1) sleep_ms(1000);  // Loop de erro
    }

    while (true) {
        
        bool joystick = !gpio_get(JOY_SW);
        // Leitura do joystick
        uint16_t x = read_adc(1);
        uint16_t y = read_adc(0);

        char msg[128];
        snprintf(msg, sizeof(msg),
            "VRX=%u VRY=%u",
            x, y
        );
        send_udp_message(msg);
        //step++;
        sleep_ms(100);
    }
}