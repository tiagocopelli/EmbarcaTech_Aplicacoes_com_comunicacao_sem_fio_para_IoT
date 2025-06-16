#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"

// =================================================================================
// ==== CONFIGURAÇÕES GERAIS ====
// =================================================================================
#define WIFI_SSID "copelli4"
#define WIFI_PASSWORD "copelli4"
#define IP_SERVIDOR "34.127.94.4" // IP do seu servidor na nuvem
#define PORTA_TCP 8082

// =================================================================================
// ==== DEFINIÇÃO DE PINOS ====
// =================================================================================
// LEDs de estado
#define LED_WIFI_CONECTADO 11
#define LED_WIFI_ERRO 12
#define LED_ESTADO 13

// Periféricos de entrada
#define PINO_JOY_VRX 27      // Eixo X do Joystick (ADC 1)
#define PINO_JOY_VRY 26      // Eixo Y do Joystick (ADC 0)
#define PINO_JOY_BOTAO 22    // Botão do Joystick
#define PINO_BOTAO_A 5       // Botão extra 'A'
#define PINO_BOTAO_B 6       // Botão extra 'B'
#define PINO_DHT 16          // Pino de dados do sensor DHT11

// =================================================================================
// ==== BIBLIOTECA DO SENSOR DHT11 (Integrada) ====
// =================================================================================

#define TIMEOUT_DHT 200

// Estrutura para guardar os resultados da leitura do DHT
typedef struct {
    float umidade;
    float temperatura_c;
    bool valido; // Flag para indicar se a leitura foi bem-sucedida
} resultado_dht_t;

// Função interna para esperar uma mudança no estado do pino
static int aguardar_nivel_pino(uint pino_gpio, bool nivel, uint timeout_us) {
    uint contador = 0;
    while (gpio_get(pino_gpio) != nivel) {
        if (contador++ > timeout_us) return 0; // Timeout
        sleep_us(1);
    }
    return contador;
}

// Função para inicializar o pino do DHT
static void dht_inicializar(uint pino_gpio) {
    gpio_init(pino_gpio);
}

// Função para ler os dados do sensor (bloqueante)
static resultado_dht_t dht_ler_bloqueante(uint pino_gpio) {
    uint8_t dados[5] = {0, 0, 0, 0, 0};
    resultado_dht_t resultado = {0.0f, 0.0f, false};

    // 1. Sinal de início enviado pelo Pico
    gpio_set_dir(pino_gpio, GPIO_OUT);
    gpio_put(pino_gpio, 0);
    sleep_ms(20); // Espera pelo menos 18ms
    gpio_put(pino_gpio, 1);
    sleep_us(40);
    gpio_set_dir(pino_gpio, GPIO_IN);

    // 2. Espera pela resposta do DHT
    if (!aguardar_nivel_pino(pino_gpio, 0, TIMEOUT_DHT)) return resultado;
    if (!aguardar_nivel_pino(pino_gpio, 1, TIMEOUT_DHT)) return resultado;
    if (!aguardar_nivel_pino(pino_gpio, 0, TIMEOUT_DHT)) return resultado;

    // 3. Leitura dos 40 bits de dados
    for (int i = 0; i < 40; i++) {
        aguardar_nivel_pino(pino_gpio, 1, TIMEOUT_DHT);
        uint contagem_baixo = aguardar_nivel_pino(pino_gpio, 0, TIMEOUT_DHT);
        if (contagem_baixo > 50) { // Se o pulso em nível alto for longo, é bit '1'
            dados[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    // 4. Verificação do checksum
    if (((dados[0] + dados[1] + dados[2] + dados[3]) & 0xFF) != dados[4]) {
        printf("Erro de checksum do DHT\n");
        return resultado; // Checksum inválido
    }
    
    // 5. Processamento dos dados
    resultado.umidade = (float)dados[0] + (float)dados[1] / 10.0f;
    resultado.temperatura_c = (float)dados[2] + (float)dados[3] / 10.0f;
    resultado.valido = true;

    return resultado;
}


// =================================================================================
// ==== LÓGICA DE CONEXÃO TCP (LWIP) ====
// =================================================================================

// Estrutura para manter o estado da conexão TCP
typedef struct CLIENTE_TCP_T_ {
    struct tcp_pcb *pcb_tcp;
    ip_addr_t endereco_remoto;
    bool conectado;
} cliente_tcp_t;

// Protótipos das funções de callback TCP
err_t callback_cliente_tcp_conectado(void *arg, struct tcp_pcb *tpcb, err_t erro);
void callback_cliente_tcp_erro(void *arg, err_t erro);
err_t callback_cliente_tcp_enviado(void *arg, struct tcp_pcb *tpcb, u16_t tamanho);
void cliente_tcp_fechar_conexao(cliente_tcp_t *estado);


// Função para enviar os dados via TCP
void cliente_tcp_enviar_dados(cliente_tcp_t *estado, const char *mensagem) {
    if (!estado->conectado || estado->pcb_tcp == NULL) {
        printf("Não conectado. Impossível enviar dados.\n");
        return;
    }
    
    printf("Enviando: %s", mensagem); // a mensagem já tem \n
    err_t erro = tcp_write(estado->pcb_tcp, mensagem, strlen(mensagem), TCP_WRITE_FLAG_COPY);

    if (erro != ERR_OK) {
        printf("Erro ao escrever para o buffer TCP: %d\n", erro);
        return;
    }
    
    erro = tcp_output(estado->pcb_tcp);
    if (erro != ERR_OK) {
        printf("Erro ao enviar dados TCP: %d\n", erro);
    }
}

// Função para fechar a conexão TCP
void cliente_tcp_fechar_conexao(cliente_tcp_t *estado) {
    if (estado->pcb_tcp != NULL) {
        tcp_arg(estado->pcb_tcp, NULL);
        tcp_sent(estado->pcb_tcp, NULL);
        tcp_err(estado->pcb_tcp, NULL);
        tcp_close(estado->pcb_tcp);
        estado->pcb_tcp = NULL;
        estado->conectado = false;
        gpio_put(LED_ESTADO, 0);
        printf("Conexão TCP fechada.\n");
    }
}

// Callback de erro
void callback_cliente_tcp_erro(void *arg, err_t erro) {
    cliente_tcp_t *estado = (cliente_tcp_t*)arg;
    printf("Erro TCP: %d. Fechando conexão.\n", erro);
    cliente_tcp_fechar_conexao(estado);
}

// Callback de dados enviados
err_t callback_cliente_tcp_enviado(void *arg, struct tcp_pcb *pcb_tcp, u16_t tamanho) {
    gpio_put(LED_ESTADO, 1); // Pisca o LED para indicar envio
    sleep_ms(50);
    gpio_put(LED_ESTADO, 0);
    return ERR_OK;
}

// Callback de conexão estabelecida
err_t callback_cliente_tcp_conectado(void *arg, struct tcp_pcb *pcb_tcp, err_t erro) {
    cliente_tcp_t *estado = (cliente_tcp_t*)arg;
    if (erro != ERR_OK) {
        printf("Falha na conexão TCP: %d\n", erro);
        cliente_tcp_fechar_conexao(estado);
        return erro;
    }
    estado->conectado = true;
    printf("Conexão TCP estabelecida com sucesso!\n");
    
    // Configura os outros callbacks
    tcp_sent(pcb_tcp, callback_cliente_tcp_enviado);
    
    // Envia uma mensagem inicial
    cliente_tcp_enviar_dados(estado, "Olá do RP2040!\n");
    return ERR_OK;
}

// Função para iniciar a conexão TCP
bool cliente_tcp_conectar(cliente_tcp_t *estado) {
    printf("Iniciando conexão com %s:%d\n", ip4addr_ntoa(&estado->endereco_remoto), PORTA_TCP);
    
    estado->pcb_tcp = tcp_new_ip_type(IP_GET_TYPE(&estado->endereco_remoto));
    if (estado->pcb_tcp == NULL) {
        printf("Erro ao criar PCB.\n");
        return false;
    }

    tcp_arg(estado->pcb_tcp, estado);
    tcp_err(estado->pcb_tcp, callback_cliente_tcp_erro);

    err_t erro = tcp_connect(estado->pcb_tcp, &estado->endereco_remoto, PORTA_TCP, callback_cliente_tcp_conectado);
    return erro == ERR_OK;
}


// =================================================================================
// ==== FUNÇÕES DE INICIALIZAÇÃO DE HARDWARE ====
// =================================================================================
void inicializar_leds() {
    gpio_init(LED_WIFI_CONECTADO);
    gpio_init(LED_WIFI_ERRO);
    gpio_init(LED_ESTADO);
    gpio_set_dir(LED_WIFI_CONECTADO, GPIO_OUT);
    gpio_set_dir(LED_WIFI_ERRO, GPIO_OUT);
    gpio_set_dir(LED_ESTADO, GPIO_OUT);
    gpio_put(LED_WIFI_CONECTADO, 0);
    gpio_put(LED_WIFI_ERRO, 0);
    gpio_put(LED_ESTADO, 0);
}

void inicializar_perifericos() {
    // ADC para o Joystick
    adc_init();
    adc_gpio_init(PINO_JOY_VRX);
    adc_gpio_init(PINO_JOY_VRY);

    // Botões com pull-up interno
    gpio_init(PINO_JOY_BOTAO);
    gpio_set_dir(PINO_JOY_BOTAO, GPIO_IN);
    gpio_pull_up(PINO_JOY_BOTAO);

    gpio_init(PINO_BOTAO_A);
    gpio_set_dir(PINO_BOTAO_A, GPIO_IN);
    gpio_pull_up(PINO_BOTAO_A);

    gpio_init(PINO_BOTAO_B);
    gpio_set_dir(PINO_BOTAO_B, GPIO_IN);
    gpio_pull_up(PINO_BOTAO_B);
    
    // Sensor DHT
    dht_inicializar(PINO_DHT);
}

uint16_t ler_adc(uint canal) {
    adc_select_input(canal);
    return adc_read();
}

bool conectar_wifi() {
    printf("Conectando ao Wi-Fi...\n");
    if (cyw43_arch_init()) {
        printf("Erro ao inicializar Wi-Fi\n");
        gpio_put(LED_WIFI_ERRO, 1);
        return false;
    }
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 20000)) {
        printf("Falha na conexão Wi-Fi\n");
        gpio_put(LED_WIFI_ERRO, 1);
        return false;
    }
    printf("Conectado! IP: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));
    gpio_put(LED_WIFI_CONECTADO, 1);
    return true;
}

// =================================================================================
// ==== FUNÇÃO PRINCIPAL (MAIN) ====
// =================================================================================
int main() {
    stdio_init_all();
    
    inicializar_leds();
    inicializar_perifericos();

    if (!conectar_wifi()) {
        while (1) { tight_loop_contents(); } // Loop infinito em caso de falha no Wi-Fi
    }

    // Prepara o estado do cliente TCP
    cliente_tcp_t *estado_tcp = calloc(1, sizeof(cliente_tcp_t));
    if (!estado_tcp) {
        printf("Erro ao alocar estado TCP\n");
        return 1;
    }
    ipaddr_aton(IP_SERVIDOR, &estado_tcp->endereco_remoto);
    
    // Variáveis para guardar os dados dos sensores
    float temperatura = 0.0f;
    float umidade = 0.0f;

    while (true) {
        if (!estado_tcp->conectado) {
            printf("Tentando conectar...\n");
            cliente_tcp_conectar(estado_tcp);
            sleep_ms(3000); // Espera 3 segundos antes de tentar de novo
        } else {
            // Se conectado, lê sensores e envia dados

            // Lê os botões (lógica invertida por causa do pull-up)
            bool botaoJoystick = !gpio_get(PINO_JOY_BOTAO);
            bool botaoA = !gpio_get(PINO_BOTAO_A);
            bool botaoB = !gpio_get(PINO_BOTAO_B);

            // Lê os eixos do Joystick
            uint16_t x = ler_adc(1); // ADC 1 -> PINO_JOY_VRX
            uint16_t y = ler_adc(0); // ADC 0 -> PINO_JOY_VRY

            // Lê os dados do sensor DHT11
            resultado_dht_t dados_dht = dht_ler_bloqueante(PINO_DHT);
            if (dados_dht.valido) {
                temperatura = dados_dht.temperatura_c;
                umidade = dados_dht.umidade;
            } else {
                printf("Falha na leitura do DHT11. Usando valores antigos.\n");
            }
            
            // Monta a string de dados
            char mensagem[256];
            snprintf(mensagem, sizeof(mensagem), "VRX=%u VRY=%u BTN=%d A=%d B=%d TEMP=%.1f UMI=%.1f\n", 
                     x, y, botaoJoystick, botaoA, botaoB, temperatura, umidade);
            
            cliente_tcp_enviar_dados(estado_tcp, mensagem);
            
            // Espera antes da próxima leitura. O DHT11 não deve ser lido mais de uma vez a cada 2s.
            sleep_ms(2000);
        }
    }
}