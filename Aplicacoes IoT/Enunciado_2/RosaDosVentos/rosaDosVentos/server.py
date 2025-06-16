import socket
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.image as mpimg

# Configurações do socket UDP para receber dados do joystick
IP_UDP = "0.0.0.0"      # Escuta em todas as interfaces de rede
PORTA_UDP = 8081        # Porta para escutar os dados UDP

# Criação do socket UDP
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((IP_UDP, PORTA_UDP))  # Vincula socket ao IP e porta
sock.setblocking(False)          # Configura socket para modo não bloqueante

def vrx_vry_para_direcao(vrx, vry):
    """
    Converte valores brutos VRX e VRY do joystick em direção da rosa dos ventos.

    Parâmetros:
        vrx (int): valor do eixo X do joystick (0 a 4095)
        vry (int): valor do eixo Y do joystick (0 a 4095)

    Retorna:
        (str, float): direção (ex: "N", "NE", etc.) e ângulo em radianos,
                      ajustado para 0 rad ser Norte e sentido horário
    """
    # Normaliza VRX e VRY para intervalo -1 a 1, com 0 no centro do joystick
    x = (vrx - 2048) / 2048.0
    y = (vry - 2048) / 2048.0

    # Calcula ângulo do vetor (x,y) usando arctan2
    angulo = np.arctan2(y, x)

    # Ajusta ângulo para que 0 rad seja no Norte e ângulo cresça no sentido horário
    angulo = (np.pi/2 - angulo) % (2*np.pi)

    # Define setores da rosa dos ventos (8 direções principais)
    setores = ["N", "NE", "L", "SE", "S", "SO", "O", "NO"]  # Norte, Nordeste, Leste, Sudeste, Sul, Sudoeste, Oeste, Noroeste
    # Calcula índice do setor a partir do ângulo
    indice_setor = int((angulo + np.pi/8) // (np.pi/4)) % 8

    return setores[indice_setor], angulo

# Cria a figura principal do matplotlib para exibir a rosa dos ventos e a seta
figura = plt.figure(figsize=(6,6))

# 1) Eixo cartesiano para a imagem de fundo (rosa dos ventos)
eixo_imagem = figura.add_axes([0, 0, 1, 1], zorder=0)
imagem_fundo = mpimg.imread('ROSA DOS VENTOS.jpg')  # Carrega imagem quadrada da rosa dos ventos
eixo_imagem.imshow(imagem_fundo)
eixo_imagem.axis('off')  # Remove os eixos para mostrar só a imagem

# 2) Eixo polar transparente para desenhar a seta da direção do joystick sobre a imagem
eixo_polar = figura.add_axes([0, 0, 1, 1], polar=True, zorder=1)
eixo_polar.set_theta_zero_location('N')   # Define zero graus apontando para cima (Norte)
eixo_polar.set_theta_direction(-1)        # Faz ângulo crescer no sentido horário
eixo_polar.set_rlim(0, 1)                  # Limita raio do gráfico polar entre 0 e 1

# Torna fundo do eixo polar transparente para imagem ficar visível
eixo_polar.patch.set_alpha(0)

# Define as direções fixas para anotação da rosa dos ventos (em graus e rótulos)
graus_direcoes = np.arange(0, 360, 45)
rotulos_direcoes = ["N", "NE", "L", "SE", "S", "SO", "O", "NO"]  # Norte, Nordeste, Leste, Sudeste, Sul, Sudoeste, Oeste, Noroeste

# Coloca as anotações das direções em torno do círculo, levemente fora do raio 1
for grau, rotulo in zip(graus_direcoes, rotulos_direcoes):
    eixo_polar.annotate(rotulo,
                        (np.deg2rad(grau), 1.05),  # posição em radianos e raio levemente maior que 1
                        ha='center',               # alinha texto horizontalmente no centro
                        va='center',               # alinha texto verticalmente no centro
                        fontsize=12,
                        fontweight='bold')

# Cria uma linha (seta) que indicará a direção do joystick
linha_seta, = eixo_polar.plot([], [], color='r', lw=3, marker='>', markersize=10)

def interpretar_mensagem(mensagem):
    """
    Interpreta mensagem recebida via UDP e extrai valores VRX e VRY.

    Parâmetros:
        mensagem (str): string no formato 'VRX=xxxx VRY=xxxx'

    Retorna:
        (int, int): valores inteiros de VRX e VRY; retorna (None, None) se falhar
    """
    try:
        partes = mensagem.strip().split()
        vrx = int(partes[0].split('=')[1])
        vry = int(partes[1].split('=')[1])
        return vrx, vry
    except:
        return None, None

def atualizar_seta(angulo):
    """
    Atualiza a posição da seta no gráfico polar conforme o ângulo recebido.

    Parâmetros:
        angulo (float): ângulo em radianos para onde a seta deve apontar
    """
    raio = [0, 0.9]            # Seta vai do centro (0) até 90% do raio
    theta = [angulo, angulo]   # Mesma direção para início e fim da linha
    linha_seta.set_data(theta, raio)

print(f"Escutando dados UDP em {IP_UDP}:{PORTA_UDP}...")

def loop_principal():
    """
    Loop principal que fica escutando mensagens UDP, processa dados do joystick,
    atualiza o gráfico e controla a exibição da seta conforme o movimento.
    """
    while True:
        try:
            # Tenta receber dados do socket UDP
            dados, endereco = sock.recvfrom(1024)
            mensagem = dados.decode()
            vrx, vry = interpretar_mensagem(mensagem)

            # Ignora se não conseguiu interpretar dados
            if vrx is None or vry is None:
                continue

            # Normaliza os valores VRX e VRY para intervalo [-1, 1]
            x = (vrx - 2048) / 2048.0
            y = (vry - 2048) / 2048.0

            # Calcula magnitude do vetor joystick para detectar zona morta
            zona_morta = 0.1  # Ajuste para ignorar pequenas oscilações
            magnitude = (x**2 + y**2)**0.5

            if magnitude < zona_morta:
                # Joystick parado, esconde a seta do gráfico
                linha_seta.set_visible(False)
            else:
                # Joystick em movimento, mostra seta e atualiza direção
                linha_seta.set_visible(True)
                direcao, angulo = vrx_vry_para_direcao(vrx, vry)
                print(f"Direção joystick: {direcao} (ângulo {np.rad2deg(angulo):.1f}°) VRX={vrx} VRY={vry}")
                atualizar_seta(angulo)

            plt.pause(0.01)  # Atualiza gráfico com pequeno delay para não travar interface

        except BlockingIOError:
            # Nenhum dado recebido no momento, apenas pausa para não travar o loop
            plt.pause(0.01)
        except Exception as e:
            print(f"Erro inesperado: {e}")
            plt.pause(0.01)

if __name__ == "__main__":
    plt.ion()    # Ativa modo interativo do matplotlib para atualização em tempo real
    plt.show()   # Exibe a janela do gráfico
    loop_principal()
