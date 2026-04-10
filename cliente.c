#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#define PORTA 5000
#define BUFFER_TAM 4096

/* ============================================================
   VARIAVEIS GLOBAIS
   ============================================================ */
static char g_resto[BUFFER_TAM] = {0};
static int  g_resto_len = 0;

/* ============================================================
   PROTOCOLO
   ============================================================ */
int enviar_linha(SOCKET sock, const char *linha) {
    int len = (int)strlen(linha);
    int enviado = send(sock, linha, len, 0);
    if (enviado <= 0) return -1;
    send(sock, "\n", 1, 0);
    return 0;
}

int receber_linha(SOCKET sock, char *buf, int tam) {
    /* Verifica se ja tem linha no buffer */
    for (int i = 0; i < g_resto_len; i++) {
        if (g_resto[i] == '\n') {
            memcpy(buf, g_resto, i);
            buf[i] = '\0';
            if (i > 0 && buf[i-1] == '\r') buf[i-1] = '\0';
            int sobra = g_resto_len - i - 1;
            if (sobra > 0) memmove(g_resto, g_resto + i + 1, sobra);
            g_resto_len = sobra;
            return (int)strlen(buf);
        }
    }
    while (1) {
        int espaco = BUFFER_TAM - g_resto_len - 1;
        if (espaco <= 0) return -1;
        int n = recv(sock, g_resto + g_resto_len, espaco, 0);
        if (n <= 0) return n;
        g_resto_len += n;
        for (int i = 0; i < g_resto_len; i++) {
            if (g_resto[i] == '\n') {
                memcpy(buf, g_resto, i);
                buf[i] = '\0';
                if (i > 0 && buf[i-1] == '\r') buf[i-1] = '\0';
                int sobra = g_resto_len - i - 1;
                if (sobra > 0) memmove(g_resto, g_resto + i + 1, sobra);
                g_resto_len = sobra;
                return (int)strlen(buf);
            }
        }
    }
}

/* Verifica se ha dados prontos no socket (non-blocking) */
int dados_disponiveis(SOCKET sock) {
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    return select(0, &fds, NULL, NULL, &tv) > 0;
}

/* ============================================================
   DESESCAPAR
   ============================================================ */
void desescapar(char *str) {
    int i = 0, j = 0;
    while (str[i]) {
        if (str[i] == '\\' && str[i+1] == 'n') {
            str[j++] = '\n'; i += 2;
        } else if (str[i] == '\\' && str[i+1] == '\\') {
            str[j++] = '\\'; i += 2;
        } else {
            str[j++] = str[i++];
        }
    }
    str[j] = '\0';
}

/* ============================================================
   EXIBICAO
   ============================================================ */
void exibir_resposta(const char *raw) {
    char copia[BUFFER_TAM];
    strncpy(copia, raw, sizeof(copia) - 1);
    copia[sizeof(copia) - 1] = '\0';
    desescapar(copia);

    char *sep = strchr(copia, '|');
    if (!sep) { printf("  %s\n", copia); return; }

    *sep = '\0';
    char *tipo = copia;
    char *conteudo = sep + 1;

    if (strcmp(tipo, "BEMVINDO") == 0) {
        char *v = strchr(conteudo, '|');
        if (v) { *v = '\0'; v++; } else { v = "?"; }
        printf("\n  ==================================================\n");
        printf("  %s\n", conteudo);
        printf("  Protocolo v%s\n", v);
        printf("  ==================================================\n\n");
    }
    else if (strcmp(tipo, "LOGIN_OK") == 0) {
        char *p1 = strchr(conteudo, '|');
        if (p1) {
            *p1 = '\0';
            char *p2 = strchr(p1 + 1, '|');
            if (p2) {
                *p2 = '\0';
                printf("\n  [OK] %s\n", p2 + 1);
                if (strcmp(p1 + 1, "admin") == 0)
                    printf("  Voce tem privilegios de administrador\n");
            }
        }
        printf("\n");
    }
    else if (strcmp(tipo, "RESULTADO") == 0) {
        char *r = strchr(conteudo, '|');
        if (r) { *r = '\0'; r++; }
        else r = conteudo;
        printf("  %s = %s\n", conteudo, r);
    }
    else if (strcmp(tipo, "ERRO") == 0 || strcmp(tipo, "ERRO_CALC") == 0) {
        printf("  [ERRO] %s\n", conteudo);
    }
    else if (strcmp(tipo, "INFO") == 0) {
        printf("  %s\n", conteudo);
    }
    else if (strcmp(tipo, "MODO") == 0) {
        printf("  Modo alterado para: %s\n", conteudo);
    }
    else if (strcmp(tipo, "AJUDA") == 0) {
        printf("%s\n", conteudo);
    }
    else if (strcmp(tipo, "HISTORICO") == 0) {
        printf("\n  === Historico ===\n%s\n", conteudo);
    }
    else if (strcmp(tipo, "STATUS") == 0) {
        printf("\n  === Status do servidor ===\n  %s\n\n", conteudo);
    }
    else if (strcmp(tipo, "ADMIN") == 0) {
        printf("\n  === Admin ===\n%s\n", conteudo);
    }
    else if (strcmp(tipo, "BROADCAST") == 0) {
        char *msg = strchr(conteudo, '|');
        if (msg) { *msg = '\0'; msg++; }
        else msg = conteudo;
        printf("\n  [BROADCAST de %s] %s\n", conteudo, msg);
    }
    else {
        printf("  %s: %s\n", tipo, conteudo);
    }
}

/* Drena mensagens pendentes (broadcasts que chegaram enquanto esperava input) */
void drenar_pendentes(SOCKET sock) {
    char buf[BUFFER_TAM];
    while (g_resto_len > 0 || dados_disponiveis(sock)) {
        int n = 0;
        /* Tenta extrair do buffer primeiro */
        int tem_linha = 0;
        for (int i = 0; i < g_resto_len; i++) {
            if (g_resto[i] == '\n') { tem_linha = 1; break; }
        }
        if (tem_linha) {
            n = receber_linha(sock, buf, sizeof(buf));
            if (n > 0) exibir_resposta(buf);
            else break;
        } else if (dados_disponiveis(sock)) {
            n = receber_linha(sock, buf, sizeof(buf));
            if (n > 0) exibir_resposta(buf);
            else break;
        } else {
            break;
        }
    }
}

/* ============================================================
   MAIN
   ============================================================ */
int main(int argc, char *argv[]) {
    char ip_buf[64];

    SetConsoleOutputCP(65001);

    printf("\n  ==================================================\n");
    printf("  Cliente - Servidor de Calculo Distribuido SPD\n");
    printf("  ==================================================\n\n");

    if (argc > 1) {
        strncpy(ip_buf, argv[1], sizeof(ip_buf) - 1);
        ip_buf[sizeof(ip_buf) - 1] = '\0';
    } else {
        printf("  Digite o IP do servidor: ");
        fflush(stdout);
        if (!fgets(ip_buf, sizeof(ip_buf), stdin)) return 1;
        ip_buf[strcspn(ip_buf, "\r\n")] = '\0';
        if (strlen(ip_buf) == 0) {
            printf("  IP nao informado. Encerrando.\n");
            return 1;
        }
    }

    const char *ip_servidor = ip_buf;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Erro ao inicializar Winsock\n");
        return 1;
    }

    printf("\n  Conectando a %s:%d...\n", ip_servidor, PORTA);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("  Erro ao criar socket\n");
        WSACleanup();
        return 1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORTA);

    if (inet_pton(AF_INET, ip_servidor, &serv_addr.sin_addr) <= 0) {
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(ip_servidor, NULL, &hints, &res) != 0) {
            printf("  Endereco invalido: %s\n", ip_servidor);
            closesocket(sock);
            WSACleanup();
            return 1;
        }
        serv_addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        printf("  Nao foi possivel conectar. O servidor esta rodando?\n");
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    char buf[BUFFER_TAM];

    /* Boas-vindas */
    int n = receber_linha(sock, buf, sizeof(buf));
    if (n <= 0) { printf("  Erro ao receber boas-vindas\n"); closesocket(sock); WSACleanup(); return 1; }
    exibir_resposta(buf);

    /* --- Autenticacao --- */
    int autenticado = 0;
    char modo_atual[20] = "basico";

    while (!autenticado) {
        printf("  1 - Login\n");
        printf("  2 - Registrar\n");
        printf("  0 - Sair\n\n");
        printf("  Opcao: ");
        fflush(stdout);

        char opcao[10];
        if (!fgets(opcao, sizeof(opcao), stdin)) break;
        opcao[strcspn(opcao, "\r\n")] = '\0';

        if (strcmp(opcao, "0") == 0) {
            closesocket(sock);
            WSACleanup();
            return 0;
        }

        char usuario[50], senha[50];

        if (strcmp(opcao, "1") == 0) {
            printf("  Usuario: "); fflush(stdout);
            fgets(usuario, sizeof(usuario), stdin);
            usuario[strcspn(usuario, "\r\n")] = '\0';
            printf("  Senha: "); fflush(stdout);
            fgets(senha, sizeof(senha), stdin);
            senha[strcspn(senha, "\r\n")] = '\0';

            char msg[256];
            snprintf(msg, sizeof(msg), "LOGIN|%s|%s", usuario, senha);
            enviar_linha(sock, msg);
        }
        else if (strcmp(opcao, "2") == 0) {
            printf("  Novo usuario: "); fflush(stdout);
            fgets(usuario, sizeof(usuario), stdin);
            usuario[strcspn(usuario, "\r\n")] = '\0';
            printf("  Nova senha: "); fflush(stdout);
            fgets(senha, sizeof(senha), stdin);
            senha[strcspn(senha, "\r\n")] = '\0';

            char msg[256];
            snprintf(msg, sizeof(msg), "REGISTRO|%s|%s", usuario, senha);
            enviar_linha(sock, msg);
        }
        else {
            printf("  Opcao invalida\n\n");
            continue;
        }

        n = receber_linha(sock, buf, sizeof(buf));
        if (n <= 0) { printf("  Erro de conexao\n"); break; }
        exibir_resposta(buf);

        if (strncmp(buf, "LOGIN_OK|", 9) == 0) {
            autenticado = 1;
        }
    }

    if (!autenticado) {
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("  Digite 'ajuda' para ver comandos ou uma expressao para calcular\n\n");

    /* --- Loop principal --- */
    char entrada[BUFFER_TAM];

    while (1) {
        /* Drena broadcasts que chegaram */
        drenar_pendentes(sock);

        printf("  [%s] > ", modo_atual);
        fflush(stdout);

        if (!fgets(entrada, sizeof(entrada), stdin)) break;
        entrada[strcspn(entrada, "\r\n")] = '\0';

        if (strlen(entrada) == 0) continue;

        /* Verifica se eh comando */
        char primeira[20] = {0};
        sscanf(entrada, "%19s", primeira);

        char lower[20];
        strncpy(lower, primeira, sizeof(lower) - 1);
        lower[sizeof(lower) - 1] = '\0';
        for (int i = 0; lower[i]; i++)
            if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] += 32;

        const char *comandos[] = {
            "sair", "ajuda", "modo", "historico", "limpar",
            "status", "listar_usuarios", "sessoes", "broadcast", "kick", NULL
        };

        int eh_comando = 0;
        for (int i = 0; comandos[i]; i++) {
            if (strcmp(lower, comandos[i]) == 0) { eh_comando = 1; break; }
        }

        if (eh_comando) {
            char msg[BUFFER_TAM];
            snprintf(msg, sizeof(msg), "CMD|%s", entrada);
            enviar_linha(sock, msg);

            if (strcmp(lower, "sair") == 0) {
                /* Espera resposta de despedida */
                n = receber_linha(sock, buf, sizeof(buf));
                if (n > 0) exibir_resposta(buf);
                break;
            }

            /* Atualiza modo local */
            if (strcmp(lower, "modo") == 0) {
                char novo_modo[20] = {0};
                sscanf(entrada, "%*s %19s", novo_modo);
                for (int i = 0; novo_modo[i]; i++)
                    if (novo_modo[i] >= 'A' && novo_modo[i] <= 'Z') novo_modo[i] += 32;
                if (strcmp(novo_modo, "basico") == 0 || strcmp(novo_modo, "cientifico") == 0 ||
                    strcmp(novo_modo, "estatistico") == 0 || strcmp(novo_modo, "conversao") == 0) {
                    strcpy(modo_atual, novo_modo);
                }
            }
        }
        else {
            char msg[BUFFER_TAM];
            snprintf(msg, sizeof(msg), "CALC|%s", entrada);
            enviar_linha(sock, msg);
        }

        /* Espera resposta do servidor */
        n = receber_linha(sock, buf, sizeof(buf));
        if (n <= 0) {
            printf("  Conexao com o servidor perdida.\n");
            break;
        }
        exibir_resposta(buf);
    }

    closesocket(sock);
    WSACleanup();
    printf("  Desconectado.\n");
    return 0;
}
