import asyncio
import websockets
import json
from datetime import datetime

# --- CONFIGURAÇÕES ---
PORTA_TCP = 8082
PORTA_WEBSOCKET = 8083
ARQUIVO_LOG = "log_servidor.txt"

CLIENTES_WEB_CONECTADOS = set()

def log(mensagem):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    log_completo = f"[{timestamp}] {mensagem}"
    print(log_completo)
    with open(ARQUIVO_LOG, "a") as f:
        f.write(log_completo + "\n")

# --- FUNÇÃO CORRIGIDA ---
async def broadcast_para_web(mensagem):
    """ Envia a mensagem para todos os clientes web conectados usando asyncio.gather. """
    if CLIENTES_WEB_CONECTADOS:
        # Cria uma tarefa para cada corotina de envio
        tasks = [asyncio.create_task(cliente.send(mensagem)) for cliente in CLIENTES_WEB_CONECTADOS]
        # Executa todas as tarefas de envio em paralelo
        await asyncio.gather(*tasks, return_exceptions=True)

async def manipulador_websocket(websocket, path):
    CLIENTES_WEB_CONECTADOS.add(websocket)
    log(f"Novo cliente web conectado. Total: {len(CLIENTES_WEB_CONECTADOS)}")
    try:
        await websocket.wait_closed()
    finally:
        CLIENTES_WEB_CONECTADOS.remove(websocket)
        log(f"Cliente web desconectou. Total: {len(CLIENTES_WEB_CONECTADOS)}")

async def manipulador_tcp(reader, writer):
    endereco_cliente = writer.get_extra_info('peername')
    log(f"RP2040 conectado de: {endereco_cliente}")
    try:
        while True:
            dados = await reader.readline()
            if not dados:
                log("RP2040 desconectou.")
                break
            
            mensagem = dados.decode('utf-8').strip()
            if not mensagem: # Ignora linhas em branco
                continue
                
            log(f"Recebido do RP2040: {mensagem}")
            
            try:
                # Verifica se a mensagem é a de boas-vindas para não tentar analisar
                if "Olá do RP2040!" in mensagem:
                    await broadcast_para_web(json.dumps({"status": "RP2040 Conectado"}))
                    continue

                dados_dict = dict(item.split('=') for item in mensagem.split(' '))
                dados_dict['VRX'] = int(dados_dict['VRX'])
                dados_dict['VRY'] = int(dados_dict['VRY'])
                await broadcast_para_web(json.dumps(dados_dict))
            except (ValueError, IndexError) as e:
                log(f"!! Erro ao analisar a mensagem: '{mensagem}', erro: {e}")

    except Exception as e:
        log(f"!! Erro na conexão TCP: {e}")
    finally:
        writer.close()
        await writer.wait_closed()
        log("Conexão com RP2040 fechada.")

async def main():
    log("Iniciando servidores...")
    servidor_tcp = await asyncio.start_server(
        manipulador_tcp, '0.0.0.0', PORTA_TCP)
    servidor_websocket = await websockets.serve(
        manipulador_websocket, "0.0.0.0", PORTA_WEBSOCKET)
    log(f"Servidor TCP rodando na porta {PORTA_TCP}")
    log(f"Servidor WebSocket rodando na porta {PORTA_WEBSOCKET}")
    await asyncio.gather(
        servidor_tcp.serve_forever(),
        servidor_websocket.serve_forever(),
    )

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nServidor encerrado manualmente.")
