#include <string.h>         // Para funções de manipulação de strings como strlen, strncmp
#include "pico/stdlib.h"     // Funções padrão do SDK do Pico (timers, stdio se usado, etc.)
#include "pico/cyw43_arch.h" // Para controle do chip Wi-Fi CYW43 (conexão Wi-Fi, LEDs da placa)
#include "hardware/gpio.h"   // Para controle direto dos pinos GPIO
#include "pico/time.h"       // Para funções de temporização (busy_wait_us, time_us_32)
#include <stdio.h>          // Necessário para sprintf/snprintf (formatação de strings)
#include "lwip/tcp.h"        // Para a pilha TCP/IP LwIP (funções do servidor TCP)

// --- Configurações Globais do Projeto ---
#define WIFI_SSID "copelli4"                // nome da sua rede Wi-Fi
#define WIFI_PASS "copelli4"                // senha da rede Wi-Fi
#define PINO_LED_ERRO 11                    // Pino GPIO para o LED de indicação de erro
#define PINO_LED_OK 12                      // Pino GPIO para o LED de indicação de status OK
#define PORTA_TCP 8081                      // Porta TCP onde o servidor web escutará por conexões
#define TIMEOUT_CONEXAO_WIFI_MS 30000       // Tempo máximo (em milissegundos) para tentar conectar ao Wi-Fi

// --- Configurações dos Pinos GPIO para Sensores ---
#define PINO_BOTAO 5                        // Pino GPIO conectado ao botão
#define PINO_DHT11 8                        // Pino GPIO conectado ao pino de dados do sensor DHT11

// --- Constantes Específicas para o Sensor DHT11 ---
#define MAX_TEMPORIZACOES_DHT 85            // Número máximo de transições de nível esperadas durante a leitura do DHT11
#define ERRO_TIMEOUT_ESPERA_NIVEL_ALVO ((uint32_t)-1)   // Valor de retorno para erro de timeout ao esperar um nível específico
#define ERRO_TIMEOUT_ESPERA_MUDANCA_NIVEL ((uint32_t)-2) // Valor de retorno para erro de timeout ao esperar mudança de nível

// --- Estrutura para Armazenar os Dados Lidos do DHT11 ---
typedef struct {
    float temperatura;      // Valor da temperatura em graus Celsius
    float umidade;          // Valor da umidade relativa em porcentagem
    bool checksum_correto; // Indica se o checksum dos dados lidos está correto
} leitura_dht11_t;

// --- HTML com CSS Embutido ---
// Esta string constante define a página web que será enviada ao navegador.
// Inclui CSS para estilização (fundo preto, texto branco, conteúdo centralizado)
// e placeholders (%s, %d, %.1f) que serão substituídos pelos dados dos sensores.
static const char *g_template_html =
    "<!DOCTYPE html><html><head><title>BitDogLab</title>"
    "<meta http-equiv=\"refresh\" content=\"1\">" // Faz a página recarregar automaticamente a cada 1 segundo
    "<style>"
    "body { background-color: #000000; color: #ffffff; font-family: Arial, sans-serif; text-align: center; padding-top: 30px; margin-left: 10px; margin-right: 10px; }"
    "h1 { color: #00c0ff; margin-bottom: 25px; }"
    "p { font-size: 1.1em; margin: 10px auto; line-height: 1.5; max-width: 500px; }"
    "span.label { font-weight: bold; color: #a0d8ef; margin-right: 8px; }"
    "span.value-ok { color: #60d060; }"        // Verde para status OK
    "span.value-fail { color: #ff6060; }"      // Vermelho para status Falha
    "span.value-pressed { color: #f0ad4e; font-weight: bold; }" // Laranja para botão pressionado
    "</style>"
    "</head><body>"
    "<h1>Status dos Sensores - BitDogLab</h1>"
    "<p><span class=\"label\">Botao (GP%d):</span><span class=\"%s\">%s</span></p>"  // Placeholders para pino, classe CSS e valor do botão
    "<p><span class=\"label\">DHT11 (GP%d):</span><span class=\"%s\">%s</span></p>"   // Placeholders para pino, classe CSS e status do DHT11
    "<p><span class=\"label\">Temperatura:</span>%.1f &deg;C</p>"                     // Placeholder para temperatura
    "<p><span class=\"label\">Umidade:</span>%.1f %%</p>"                          // Placeholder para umidade
    "</body></html>";

// --- Estado Global do Servidor TCP ---
static struct tcp_pcb *g_pcb_cliente = NULL; // Ponteiro para o Bloco de Controle de Protocolo (PCB) da conexão do cliente atual
static struct tcp_pcb *g_pcb_escuta = NULL;  // Ponteiro para o PCB do servidor que está escutando por novas conexões

// --- Funções Auxiliares ---
/**
 * Pisca um LED conectado a um pino GPIO.
 * * O número do pino GPIO onde o LED está conectado.
 * O número de vezes que o LED deve piscar.
 * O intervalo (em milissegundos) entre acender e apagar o LED.
 */
void pisca_led(uint pino_led, int vezes, int intervalo_ms) {
    for (int i = 0; i < vezes; ++i) {
        gpio_put(pino_led, 1); // Acende o LED
        sleep_ms(intervalo_ms);
        gpio_put(pino_led, 0); // Apaga o LED
        sleep_ms(intervalo_ms);
    }
}

/**
 * Função auxiliar para aguardar uma mudança de nível em um pino GPIO ou um timeout.
 * Usada primariamente pela função de leitura do DHT11.
 * O pino GPIO a ser monitorado.
 * esperar_nivel_alto Se true, espera o pino ir para ALTO (1); se false, espera ir para BAIXO (0).
 * Depois, mede quanto tempo o pino permanece nesse estado antes de mudar.
 * timeout_microsegundos Tempo máximo de espera em microssegundos.
 * uint32_t A duração (em microssegundos) que o pino permaneceu no estado `esperar_nivel_alto` antes de mudar,
 * ou um código de erro TIMEOUT se o tempo máximo for atingido.
 */
static uint32_t aguardar_mudanca_nivel_gpio(uint pino_gpio, bool esperar_nivel_alto, uint32_t timeout_microsegundos) {
    uint32_t inicio_us = time_us_32(); // Pega o tempo atual em microssegundos

    // Loop 1: Espera o pino atingir o estado `esperar_nivel_alto`
    while (gpio_get(pino_gpio) != esperar_nivel_alto) {
        if (time_us_32() - inicio_us > timeout_microsegundos) {
            return ERRO_TIMEOUT_ESPERA_NIVEL_ALVO; // Retorna erro se o timeout for atingido
        }
    }
    // Loop 2: O pino atingiu o estado `esperar_nivel_alto`. Agora mede quanto tempo ele permanece assim.
    inicio_us = time_us_32(); // Reinicia o contador de tempo
    while (gpio_get(pino_gpio) == esperar_nivel_alto) {
        if (time_us_32() - inicio_us > timeout_microsegundos) {
            return ERRO_TIMEOUT_ESPERA_MUDANCA_NIVEL; // Retorna erro se o timeout for atingido
        }
    }
    return time_us_32() - inicio_us; // Retorna a duração que o pino permaneceu no estado `esperar_nivel_alto`
}

/**
 * Lê os dados de temperatura e umidade do sensor DHT11.
 * Implementa o protocolo de comunicação "bit-banging" do DHT11.
 * resultado Ponteiro para uma struct leitura_dht11_t onde os dados lidos serão armazenados.
 * true Se a leitura e a verificação do checksum forem bem-sucedidas.
 * false Se ocorrer algum erro durante a leitura ou o checksum falhar.
 */
bool ler_dht11(leitura_dht11_t *resultado) {
    uint8_t dados_sensor[5] = {0, 0, 0, 0, 0}; // Array para armazenar os 5 bytes de dados do DHT11
    int i;

    // Inicializa a struct de resultado
    resultado->checksum_correto = false; 
    resultado->temperatura = 0.0f; // Valor padrão em caso de falha
    resultado->umidade = 0.0f;     // Valor padrão em caso de falha

    // Fase 1: Sinal de Start para o DHT11
    gpio_set_dir(PINO_DHT11, GPIO_OUT); // Configura o pino como saída
    gpio_put(PINO_DHT11, 0);            // Coloca o pino em nível baixo
    busy_wait_ms(20);                   // Mantém em baixo por 20ms (mínimo 18ms)
    gpio_put(PINO_DHT11, 1);            // Coloca o pino em nível alto
    busy_wait_us(40);                   // Mantém em alto por 40µs

    // Fase 2: Prepara para receber a resposta do DHT11
    gpio_set_dir(PINO_DHT11, GPIO_IN);  // Configura o pino como entrada

    // Fase 3: Leitura da Resposta e dos Dados do DHT11
    // O DHT11 deve responder puxando a linha para baixo (~80µs) e depois para alto (~80µs)
    if (aguardar_mudanca_nivel_gpio(PINO_DHT11, false, 120) >= ERRO_TIMEOUT_ESPERA_NIVEL_ALVO) return false; // Espera resposta LOW
    if (aguardar_mudanca_nivel_gpio(PINO_DHT11, true, 120) >= ERRO_TIMEOUT_ESPERA_NIVEL_ALVO) return false;  // Espera resposta HIGH

    // Loop para ler os 40 bits de dados (5 bytes)
    for (i = 0; i < 40; ++i) {
        // Cada bit é precedido por um pulso baixo de ~50µs
        if (aguardar_mudanca_nivel_gpio(PINO_DHT11, false, 70) >= ERRO_TIMEOUT_ESPERA_NIVEL_ALVO) return false;
        // A duração do pulso alto subsequente determina se o bit é 0 ou 1
        uint32_t duracao_alto = aguardar_mudanca_nivel_gpio(PINO_DHT11, true, 100);
        if (duracao_alto >= ERRO_TIMEOUT_ESPERA_NIVEL_ALVO) return false;
        
        dados_sensor[i / 8] <<= 1; // Desloca o byte para a esquerda para abrir espaço para o novo bit
        if (duracao_alto > 45) {   // Se o pulso alto for maior que ~45µs, considera-se bit 1 (valor empírico, pode precisar de ajuste)
            dados_sensor[i / 8] |= 1;
        }
    }
    
    // Fase 4: Verificação do Checksum
    // O checksum é a soma dos 4 primeiros bytes, comparada com o 5º byte.
    uint8_t checksum_calculado = (dados_sensor[0] + dados_sensor[1] + dados_sensor[2] + dados_sensor[3]) & 0xFF;
    if (checksum_calculado != dados_sensor[4]) {
        return false; // Checksum falhou
    }
    
    // Fase 5: Armazena os dados na struct de resultado
    // Para o DHT11, data[1] e data[3] (partes decimais) são geralmente 0.
    resultado->umidade = (float)dados_sensor[0]; 
    resultado->temperatura = (float)dados_sensor[2];
    resultado->checksum_correto = true; // Leitura bem-sucedida
    return true;
}


// --- Funções do Servidor TCP ---
/**
 * Fecha de forma segura a conexão TCP com um cliente.
 * Limpa os callbacks e fecha o PCB.
 * pcb_a_fechar O PCB da conexão do cliente a ser fechada.
 */
static void fechar_conexao_cliente(struct tcp_pcb *pcb_a_fechar) {
    if (pcb_a_fechar) {
        // Remove todos os callbacks e argumentos associados ao PCB
        tcp_arg(pcb_a_fechar, NULL);
        tcp_sent(pcb_a_fechar, NULL);
        tcp_recv(pcb_a_fechar, NULL);
        tcp_err(pcb_a_fechar, NULL);
        tcp_poll(pcb_a_fechar, NULL, 0);
        
        // Tenta fechar a conexão
        if (tcp_close(pcb_a_fechar) != ERR_OK) {
            tcp_abort(pcb_a_fechar); // Se o fechamento normal falhar, aborta a conexão
        }
    }
    // Se o PCB fechado era o cliente ativo global, zera a referência global
    if (pcb_a_fechar == g_pcb_cliente) {
        g_pcb_cliente = NULL;
    }
}

/**
 * Callback chamado pela pilha LwIP quando ocorre um erro na conexão TCP.
 * arg Argumento definido pelo usuário 
 * Código do erro LwIP.
 */
static void server_err_cb(void *arg, err_t err) {
    if (g_pcb_cliente != NULL) { // Se havia um cliente ativo
        g_pcb_cliente = NULL;    // Zera a referência, pois a conexão está com erro
    }
    pisca_led(PINO_LED_ERRO, 3, 150); // Sinaliza o erro de conexão piscando o LED
}

/**
 * Callback chamado pela pilha LwIP após os dados enviados via tcp_write()
 * serem confirmados (ACKed) pelo cliente.
 * arg Argumento definido pelo usuário.
 * tpcb O PCB da conexão.
 * len O número de bytes confirmados como enviados.
 * err_t ERR_OK se tudo correu bem.
 */
static err_t server_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    pisca_led(PINO_LED_OK, 1, 20); // Pisca LED OK rapidamente para indicar sucesso no envio
    fechar_conexao_cliente(tpcb);   // Fecha a conexão, pois a resposta foi enviada
    return ERR_OK;
}

/**
 * Callback chamado pela pilha LwIP quando dados são recebidos do cliente.
 * É aqui que a requisição HTTP GET é processada e a página HTML é gerada e enviada.
 * arg Argumento definido pelo usuário.
 * tpcb O PCB da conexão.
 * p O buffer (pbuf) contendo os dados recebidos; NULL se o cliente fechou a conexão.
 * err Código de erro LwIP.
 * err_t ERR_OK se tudo correu bem.
 */
static err_t server_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    // Trata erros na recepção ou se o cliente abortou
    if (err != ERR_OK && err != ERR_ABRT) {
        if (p) pbuf_free(p); // Libera o buffer se existir
        fechar_conexao_cliente(tpcb);
        return err;
    }

    // Se p é NULL, o cliente fechou a conexão remotamente
    if (!p) {
        fechar_conexao_cliente(tpcb);
        return ERR_OK; 
    }

    // Informa à pilha LwIP que os dados do pbuf foram processados
    tcp_recved(tpcb, p->tot_len);

    // Verifica se é uma requisição HTTP GET (verificação básica)
    if (p->tot_len >= 3 && strncmp((char *)p->payload, "GET", 3) == 0) {
        // Lê o estado atual do botão
        bool botao_esta_pressionado = !gpio_get(PINO_BOTAO); // Pull-up: pressionado = nível baixo (0)
        
        // Lê os dados atuais do sensor DHT11
        leitura_dht11_t leitura_dht_corrente;
        bool leitura_dht_foi_ok = ler_dht11(&leitura_dht_corrente);

        // Prepara strings para os valores e classes CSS dinâmicas
        char str_valor_botao[15]; char str_classe_botao[30]; 
        sprintf(str_valor_botao, botao_esta_pressionado ? "PRESSIONADO" : "SOLTO");
        sprintf(str_classe_botao, botao_esta_pressionado ? "value-pressed" : "value-ok");

        char str_status_dht[30]; char str_classe_dht[30]; 
        sprintf(str_status_dht, leitura_dht_foi_ok ? "OK" : "Falha na leitura");
        sprintf(str_classe_dht, leitura_dht_foi_ok ? "value-ok" : "value-fail");

        // Monta o corpo do HTML dinamicamente usando o template e os dados atuais
        char corpo_html_dinamico[1200]; // Buffer para o corpo HTML
        int tamanho_necessario_corpo = snprintf(corpo_html_dinamico, sizeof(corpo_html_dinamico), g_template_html,
                 PINO_BOTAO, str_classe_botao, str_valor_botao,
                 PINO_DHT11, str_classe_dht, str_status_dht,
                 leitura_dht_foi_ok ? leitura_dht_corrente.temperatura : -99.0f, // Usa -99 se falha
                 leitura_dht_foi_ok ? leitura_dht_corrente.umidade : -99.0f);   // Usa -99 se falha
        
        // Verifica se o buffer do corpo HTML foi suficiente
        if (tamanho_necessario_corpo >= sizeof(corpo_html_dinamico)) {
             // AVISO: HTML BODY TRUNCADO! O buffer é pequeno demais.
             pisca_led(PINO_LED_ERRO, 5, 100); // Pisca LED de erro
        }

        // Monta a resposta HTTP completa (cabeçalhos HTTP + corpo HTML)
        char resposta_http_completa[1400]; // Buffer para a resposta HTTP completa
        int tamanho_corpo_html = strlen(corpo_html_dinamico);
        int tamanho_resposta_http = snprintf(resposta_http_completa, sizeof(resposta_http_completa),
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/html; charset=utf-8\r\n" // Define tipo e codificação
                                "Content-Length: %d\r\n"                     // Tamanho do corpo HTML
                                "Connection: close\r\n\r\n"                  // Informa que a conexão será fechada após esta resposta
                                "%s",                                        // O corpo HTML dinâmico
                                tamanho_corpo_html, corpo_html_dinamico);

        // Envia a resposta HTTP ao cliente
        if (tamanho_resposta_http > 0 && tamanho_resposta_http < sizeof(resposta_http_completa)) {
            err_t erro_ao_escrever = tcp_write(tpcb, resposta_http_completa, tamanho_resposta_http, TCP_WRITE_FLAG_COPY);
            if (erro_ao_escrever == ERR_OK) {
                tcp_output(tpcb); // Tenta enviar os dados imediatamente
                tcp_sent(tpcb, server_sent_cb); // Define callback para quando o envio for confirmado
            } else { 
                pisca_led(PINO_LED_ERRO, 4, 100); // Erro ao tentar escrever para o socket TCP
                fechar_conexao_cliente(tpcb); 
            }
        } else { 
            pisca_led(PINO_LED_ERRO, 5, 100); // Erro ao formatar a resposta HTTP completa ou buffer pequeno
            fechar_conexao_cliente(tpcb); 
        }
    } else { // Se a requisição não for um GET
        fechar_conexao_cliente(tpcb); 
    }
    pbuf_free(p); // Libera o buffer da requisição recebida
    return ERR_OK;
}

/**
 * Callback chamado pela pilha LwIP quando uma nova conexão de cliente é aceita
 * no PCB que está escutando.
 * arg Argumento definido pelo usuário.
 * novo_pcb_cliente O PCB da nova conexão estabelecida.
 * err Código de erro LwIP.
 * err_t ERR_OK se a conexão for aceita, ou um erro LwIP caso contrário.
 */
static err_t server_accept_cb(void *arg, struct tcp_pcb *novo_pcb_cliente, err_t err) {
    // Verifica se houve erro ao aceitar ou se o novo PCB é nulo
    if (err != ERR_OK || novo_pcb_cliente == NULL) { 
        if (novo_pcb_cliente) fechar_conexao_cliente(novo_pcb_cliente); // Fecha o novo PCB se ele foi criado
        return ERR_VAL; // Retorna erro de valor inválido
    }

    // Este servidor simples lida com apenas um cliente por vez.
    // Se já houver um cliente conectado (g_pcb_cliente não é NULL), recusa a nova conexão.
    if (g_pcb_cliente != NULL) { 
        fechar_conexao_cliente(novo_pcb_cliente); // Fecha a nova conexão
        return ERR_ABRT; // Indica ao LwIP para abortar esta conexão
    }

    g_pcb_cliente = novo_pcb_cliente; // Armazena o PCB do novo cliente conectado

    // Configura os callbacks para a nova conexão do cliente
    tcp_setprio(g_pcb_cliente, TCP_PRIO_NORMAL); // Define a prioridade da conexão
    tcp_arg(g_pcb_cliente, NULL); // Define um argumento para ser passado aos callbacks (não usado aqui)
    tcp_recv(g_pcb_cliente, server_recv_cb);   // Define o callback para quando dados são recebidos
    tcp_err(g_pcb_cliente, server_err_cb);     // Define o callback para quando erros ocorrem
    return ERR_OK; // Conexão aceita com sucesso
}

/**
 * Inicializa o servidor TCP.
 * Cria um PCB, faz o bind para a porta e endereço, e começa a escutar por conexões.
 * true Se o servidor foi inicializado com sucesso.
 * false Se ocorreu algum erro durante a inicialização.
 */
bool init_servidor_tcp(void) {
    // Cria um novo PCB (Protocol Control Block) para escutar por conexões TCP
    g_pcb_escuta = tcp_new_ip_type(IPADDR_TYPE_ANY); // IPADDR_TYPE_ANY para escutar em qualquer interface de rede
    if (!g_pcb_escuta) { 
        pisca_led(PINO_LED_ERRO, 5, 200); // 5 piscadas = erro ao criar PCB
        return false; 
    }

    // Associa (bind) o PCB a qualquer endereço IP local e à porta TCP definida
    err_t erro_bind = tcp_bind(g_pcb_escuta, IP_ANY_TYPE, PORTA_TCP);
    if (erro_bind != ERR_OK) {
        fechar_conexao_cliente(g_pcb_escuta); g_pcb_escuta = NULL; // Limpa o PCB de escuta
        pisca_led(PINO_LED_ERRO, 6, 200); // 6 piscadas = erro no bind
        return false;
    }

    // Coloca o PCB no estado de escuta (LISTEN), pronto para aceitar conexões
    // O backlog de 1 significa que apenas 1 conexão pode ficar na fila se o servidor estiver ocupado.
    struct tcp_pcb *pcb_temporario_escuta = tcp_listen_with_backlog(g_pcb_escuta, 1);
    if (!pcb_temporario_escuta) { // Se tcp_listen falhar, o PCB original é liberado por LwIP
        if (g_pcb_escuta) fechar_conexao_cliente(g_pcb_escuta); // Segurança extra
        g_pcb_escuta = NULL; 
        pisca_led(PINO_LED_ERRO, 7, 200); // 7 piscadas = erro ao escutar
        return false;
    }
    g_pcb_escuta = pcb_temporario_escuta; // Atualiza para o PCB retornado por tcp_listen

    // Define a função de callback (server_accept_cb) que será chamada quando uma nova conexão for aceita
    tcp_accept(g_pcb_escuta, server_accept_cb);
    return true; // Servidor inicializado com sucesso
}


// --- Função Principal ---
int main() {

    // Inicializa os pinos GPIO para os LEDs
    gpio_init(PINO_LED_ERRO); gpio_set_dir(PINO_LED_ERRO, GPIO_OUT);
    gpio_init(PINO_LED_OK);   gpio_set_dir(PINO_LED_OK, GPIO_OUT);

    // Inicializa o pino GPIO para o botão
    gpio_init(PINO_BOTAO); 
    gpio_set_dir(PINO_BOTAO, GPIO_IN); // Configura como entrada
    gpio_pull_up(PINO_BOTAO);          // Habilita resistor de pull-up interno
    
    // Inicializa o pino GPIO para o sensor DHT11
    gpio_init(PINO_DHT11); // A direção (entrada/saída) será gerenciada pela função ler_dht11()

    // Inicializa o chip Wi-Fi CYW43
    if (cyw43_arch_init()) {
        // Erro crítico: não conseguiu inicializar o hardware Wi-Fi
        while (true) pisca_led(PINO_LED_ERRO, 1, 500); // Pisca LED de erro continuamente
    }
    cyw43_arch_enable_sta_mode(); // Habilita o modo "Station" (para conectar a um roteador Wi-Fi)
    
    // Tenta conectar à rede Wi-Fi especificada
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, TIMEOUT_CONEXAO_WIFI_MS)) {
        // Erro: não conseguiu conectar ao Wi-Fi
        while (true) pisca_led(PINO_LED_ERRO, 2, 700); // Pisca LED de erro continuamente
    }
    // Se chegou aqui, o Wi-Fi está conectado
    gpio_put(PINO_LED_OK, 1); // Acende o LED de OK para indicar que o Wi-Fi está conectado e o sistema pronto

    // Inicializa o servidor TCP
    if (!init_servidor_tcp()) {
        // Erro: não conseguiu inicializar o servidor TCP
        // A função init_servidor_tcp() já terá acionado o LED de erro com um padrão específico.
        while(true) { // Mantém o Pico funcionando para que o LED de erro continue piscando
            cyw43_arch_poll(); // Continua processando eventos de rede
            sleep_ms(100); 
        }
    }

    // Se chegou aqui, o servidor web está pronto e escutando por conexões
    // Loop principal do programa
    while (true) {
        cyw43_arch_poll(); // ESSENCIAL: Processa todos os eventos pendentes da rede Wi-Fi e da pilha TCP/IP LwIP
        sleep_ms(10);      // Pequena pausa para não sobrecarregar a CPU, mas mantém a responsividade
    }
    return 0; // Esta linha nunca é alcançada em um sistema embarcado típico
}