from datetime import datetime
import socket

def udp_server(ip="0.0.0.0", port=8081):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((ip, port))
        print(f"⚡ Servidor UDP ouvindo em {ip}:{port}")
        
        try:
            while True:
                data, addr = sock.recvfrom(1024)
                timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                try:
                    print(f"[{timestamp}] {addr[0]}:{addr[1]} -> {data.decode()}")
                except UnicodeDecodeError:
                    print(f"[{timestamp}] {addr[0]}:{addr[1]} -> Dados binários: {data.hex()}")
                    
        except KeyboardInterrupt:
            print("\n🛑 Servidor encerrado")
        except Exception as e:
            print(f"❌ Erro: {e}")

if __name__ == "__main__":
    udp_server()