#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#define PORTA 5000
#define MAX_CLIENTES 20
#define BUFFER_TAM 4096
#define MAX_USUARIOS 50
#define MAX_HISTORICO 100
#define LOG_ARQUIVO "servidor.log"

/* ============================================================
   ESTRUTURAS
   ============================================================ */
typedef struct {
    char nome[50];
    char senha[50];
    char papel[20]; /* "admin" ou "usuario" */
} Usuario;

typedef struct {
    char hora[20];
    char modo[20];
    char expressao[256];
    char resultado[256];
    int sucesso;
} RegistroCalculo;

typedef struct {
    SOCKET sock;
    char ip[50];
    char usuario[50];
    char papel[20];
    char modo[20];
    RegistroCalculo historico[MAX_HISTORICO];
    int hist_count;
    int ativo;
} Sessao;

/* ============================================================
   VARIAVEIS GLOBAIS
   ============================================================ */
static Usuario usuarios[MAX_USUARIOS];
static int num_usuarios = 0;
static CRITICAL_SECTION cs_usuarios;

static Sessao sessoes[MAX_CLIENTES];
static CRITICAL_SECTION cs_sessoes;

static int total_requisicoes = 0;
static int total_calculos = 0;
static int total_erros = 0;
static time_t inicio_servidor;
static CRITICAL_SECTION cs_estado;

static CRITICAL_SECTION cs_log;

/* ============================================================
   LOG
   ============================================================ */
void registrar_log(const char *nivel, const char *mensagem) {
    time_t agora = time(NULL);
    struct tm *t = localtime(&agora);
    char data_hora[64];
    strftime(data_hora, sizeof(data_hora), "%Y-%m-%d %H:%M:%S", t);

    EnterCriticalSection(&cs_log);
    printf("[%s] [%s] %s\n", data_hora, nivel, mensagem);

    FILE *f = fopen(LOG_ARQUIVO, "a");
    if (f) {
        fprintf(f, "[%s] [%s] %s\n", data_hora, nivel, mensagem);
        fclose(f);
    }
    LeaveCriticalSection(&cs_log);
}

/* ============================================================
   USUARIOS
   ============================================================ */
void inicializar_usuarios(void) {
    strcpy(usuarios[0].nome, "admin");
    strcpy(usuarios[0].senha, "admin123");
    strcpy(usuarios[0].papel, "admin");
    strcpy(usuarios[1].nome, "aluno");
    strcpy(usuarios[1].senha, "spd2026");
    strcpy(usuarios[1].papel, "usuario");
    num_usuarios = 2;
}

/* Retorna papel ou NULL */
const char* autenticar(const char *nome, const char *senha) {
    EnterCriticalSection(&cs_usuarios);
    for (int i = 0; i < num_usuarios; i++) {
        if (strcmp(usuarios[i].nome, nome) == 0 && strcmp(usuarios[i].senha, senha) == 0) {
            const char *papel = usuarios[i].papel;
            LeaveCriticalSection(&cs_usuarios);
            return papel;
        }
    }
    LeaveCriticalSection(&cs_usuarios);
    return NULL;
}

int registrar_usuario(const char *nome, const char *senha, char *msg_out) {
    EnterCriticalSection(&cs_usuarios);
    for (int i = 0; i < num_usuarios; i++) {
        if (strcmp(usuarios[i].nome, nome) == 0) {
            LeaveCriticalSection(&cs_usuarios);
            strcpy(msg_out, "Usuario ja existe");
            return 0;
        }
    }
    if (num_usuarios >= MAX_USUARIOS) {
        LeaveCriticalSection(&cs_usuarios);
        strcpy(msg_out, "Limite de usuarios atingido");
        return 0;
    }
    strcpy(usuarios[num_usuarios].nome, nome);
    strcpy(usuarios[num_usuarios].senha, senha);
    strcpy(usuarios[num_usuarios].papel, "usuario");
    num_usuarios++;
    LeaveCriticalSection(&cs_usuarios);
    strcpy(msg_out, "Registrado com sucesso");
    return 1;
}

/* ============================================================
   PROTOCOLO — envio/recepcao linha a linha
   ============================================================ */
int enviar_linha(SOCKET sock, const char *linha) {
    int len = (int)strlen(linha);
    int enviado = send(sock, linha, len, 0);
    if (enviado <= 0) return -1;
    /* envia \n como separador */
    send(sock, "\n", 1, 0);
    return 0;
}

/* Formato simplificado: TIPO|campo1|campo2|...
   Mais leve que JSON, facil de parsear em C */

/* Escapa \n para transmissao segura no protocolo linha-a-linha */
void escapar(const char *src, char *dst, int tam) {
    int j = 0;
    for (int i = 0; src[i] && j < tam - 2; i++) {
        if (src[i] == '\n') {
            dst[j++] = '\\'; dst[j++] = 'n';
        } else if (src[i] == '\\') {
            dst[j++] = '\\'; dst[j++] = '\\';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

void enviar_msg(SOCKET sock, const char *tipo, const char *conteudo) {
    char escapado[BUFFER_TAM];
    escapar(conteudo, escapado, sizeof(escapado));
    char buf[BUFFER_TAM];
    snprintf(buf, sizeof(buf), "%s|%s", tipo, escapado);
    enviar_linha(sock, buf);
}

/* Le uma linha completa do socket (ate \n).
   Retorna numero de bytes lidos, 0 se desconectou, -1 se erro. */
int receber_linha(SOCKET sock, char *buf, int tam, char *resto, int *resto_len) {
    /* Verifica se ja tem uma linha completa no resto */
    for (int i = 0; i < *resto_len; i++) {
        if (resto[i] == '\n') {
            memcpy(buf, resto, i);
            buf[i] = '\0';
            /* Remove \r se houver */
            if (i > 0 && buf[i-1] == '\r') buf[i-1] = '\0';
            /* Desloca o resto */
            int sobra = *resto_len - i - 1;
            if (sobra > 0) memmove(resto, resto + i + 1, sobra);
            *resto_len = sobra;
            return (int)strlen(buf);
        }
    }

    /* Precisa ler mais dados */
    while (1) {
        int espaco = BUFFER_TAM - *resto_len - 1;
        if (espaco <= 0) return -1;
        int n = recv(sock, resto + *resto_len, espaco, 0);
        if (n <= 0) return n;
        *resto_len += n;

        for (int i = 0; i < *resto_len; i++) {
            if (resto[i] == '\n') {
                memcpy(buf, resto, i);
                buf[i] = '\0';
                if (i > 0 && buf[i-1] == '\r') buf[i-1] = '\0';
                int sobra = *resto_len - i - 1;
                if (sobra > 0) memmove(resto, resto + i + 1, sobra);
                *resto_len = sobra;
                return (int)strlen(buf);
            }
        }
    }
}

/* ============================================================
   MOTOR DE CALCULO
   ============================================================ */

/* Basico: "num1 op num2" */
int calc_basico(const char *expr, char *resultado) {
    double a, b, r;
    char op[4] = {0};
    char sobra[16] = {0};

    /* Tenta operadores de 2 caracteres primeiro, verificando que nao sobra nada */
    if (sscanf(expr, "%lf ** %lf %15s", &a, &b, sobra) == 2) { strcpy(op, "**"); }
    else if (sscanf(expr, "%lf // %lf %15s", &a, &b, sobra) == 2) { strcpy(op, "//"); }
    else if (sscanf(expr, "%lf + %lf %15s", &a, &b, sobra) == 2) { strcpy(op, "+"); }
    else if (sscanf(expr, "%lf - %lf %15s", &a, &b, sobra) == 2) { strcpy(op, "-"); }
    else if (sscanf(expr, "%lf * %lf %15s", &a, &b, sobra) == 2) { strcpy(op, "*"); }
    else if (sscanf(expr, "%lf / %lf %15s", &a, &b, sobra) == 2) { strcpy(op, "/"); }
    else if (sscanf(expr, "%lf %% %lf %15s", &a, &b, sobra) == 2) { strcpy(op, "%"); }
    else {
        strcpy(resultado, "ERRO|Formato invalido. Use: num op num (ex: 2 + 3)");
        return 0;
    }

    if ((strcmp(op, "/") == 0 || strcmp(op, "//") == 0 || strcmp(op, "%") == 0) && b == 0) {
        strcpy(resultado, "ERRO|Divisao por zero");
        return 0;
    }

    if (strcmp(op, "+") == 0)       r = a + b;
    else if (strcmp(op, "-") == 0)  r = a - b;
    else if (strcmp(op, "*") == 0)  r = a * b;
    else if (strcmp(op, "/") == 0)  r = a / b;
    else if (strcmp(op, "//") == 0) r = floor(a / b);
    else if (strcmp(op, "**") == 0) {
        if (fabs(b) > 1000) { strcpy(resultado, "ERRO|Expoente muito grande"); return 0; }
        r = pow(a, b);
    }
    else if (strcmp(op, "%") == 0)  r = fmod(a, b);
    else { strcpy(resultado, "ERRO|Operador desconhecido"); return 0; }

    if (r == (long long)r && fabs(r) < 1e15)
        snprintf(resultado, 256, "%lld", (long long)r);
    else
        snprintf(resultado, 256, "%.6g", r);
    return 1;
}

/* Cientifico: "funcao(valor)" ou operacao basica */
int calc_cientifico(const char *expr, char *resultado) {
    double arg, r;
    char funcao[20];

    if (_stricmp(expr, "pi") == 0) {
        snprintf(resultado, 256, "%.15g", 3.14159265358979323846);
        return 1;
    }
    if (_stricmp(expr, "euler") == 0) {
        snprintf(resultado, 256, "%.15g", 2.71828182845904523536);
        return 1;
    }

    if (sscanf(expr, "%19[a-z](%lf)", funcao, &arg) == 2) {
        if (strcmp(funcao, "sen") == 0)        r = sin(arg);
        else if (strcmp(funcao, "cos") == 0)   r = cos(arg);
        else if (strcmp(funcao, "tan") == 0)   r = tan(arg);
        else if (strcmp(funcao, "raiz") == 0) {
            if (arg < 0) { strcpy(resultado, "ERRO|Raiz de numero negativo"); return 0; }
            r = sqrt(arg);
        }
        else if (strcmp(funcao, "log") == 0) {
            if (arg <= 0) { strcpy(resultado, "ERRO|Log de numero nao positivo"); return 0; }
            r = log10(arg);
        }
        else if (strcmp(funcao, "ln") == 0) {
            if (arg <= 0) { strcpy(resultado, "ERRO|Ln de numero nao positivo"); return 0; }
            r = log(arg);
        }
        else if (strcmp(funcao, "abs") == 0)   r = fabs(arg);
        else if (strcmp(funcao, "fat") == 0) {
            if (arg < 0 || arg != (int)arg) { strcpy(resultado, "ERRO|Fatorial: use inteiro positivo"); return 0; }
            if (arg > 170) { strcpy(resultado, "ERRO|Fatorial: valor muito grande"); return 0; }
            r = 1;
            for (int i = 2; i <= (int)arg; i++) r *= i;
        }
        else if (strcmp(funcao, "ceil") == 0)  r = ceil(arg);
        else if (strcmp(funcao, "floor") == 0) r = floor(arg);
        else if (strcmp(funcao, "graus") == 0) r = arg * 180.0 / 3.14159265358979323846;
        else if (strcmp(funcao, "rad") == 0)   r = arg * 3.14159265358979323846 / 180.0;
        else { snprintf(resultado, 256, "ERRO|Funcao desconhecida: %s", funcao); return 0; }

        if (r == (long long)r && fabs(r) < 1e15)
            snprintf(resultado, 256, "%lld", (long long)r);
        else
            snprintf(resultado, 256, "%.6g", r);
        return 1;
    }

    return calc_basico(expr, resultado);
}

/* Estatistico: "comando val1 val2 val3 ..." */
int calc_estatistico(const char *expr, char *resultado) {
    char comando[20];
    double valores[500];
    int n = 0;

    char copia[BUFFER_TAM];
    strncpy(copia, expr, sizeof(copia) - 1);
    copia[sizeof(copia) - 1] = '\0';

    char *token = strtok(copia, " ");
    if (!token) { strcpy(resultado, "ERRO|Use: comando valores (ex: media 1 2 3)"); return 0; }
    strncpy(comando, token, sizeof(comando) - 1);
    comando[sizeof(comando) - 1] = '\0';

    while ((token = strtok(NULL, " ")) != NULL && n < 500) {
        valores[n++] = atof(token);
    }

    if (n == 0) { strcpy(resultado, "ERRO|Forneca ao menos um valor"); return 0; }

    double r = 0;

    if (strcmp(comando, "soma") == 0) {
        for (int i = 0; i < n; i++) r += valores[i];
    }
    else if (strcmp(comando, "media") == 0) {
        for (int i = 0; i < n; i++) r += valores[i];
        r /= n;
    }
    else if (strcmp(comando, "min") == 0) {
        r = valores[0];
        for (int i = 1; i < n; i++) if (valores[i] < r) r = valores[i];
    }
    else if (strcmp(comando, "max") == 0) {
        r = valores[0];
        for (int i = 1; i < n; i++) if (valores[i] > r) r = valores[i];
    }
    else if (strcmp(comando, "contagem") == 0) {
        r = n;
    }
    else if (strcmp(comando, "mediana") == 0) {
        /* Ordena */
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (valores[j] < valores[i]) { double tmp = valores[i]; valores[i] = valores[j]; valores[j] = tmp; }
        if (n % 2 == 1) r = valores[n / 2];
        else r = (valores[n / 2 - 1] + valores[n / 2]) / 2.0;
    }
    else if (strcmp(comando, "desvio") == 0 || strcmp(comando, "variancia") == 0) {
        if (n < 2) { strcpy(resultado, "ERRO|Requer ao menos 2 valores"); return 0; }
        double media = 0;
        for (int i = 0; i < n; i++) media += valores[i];
        media /= n;
        double soma_sq = 0;
        for (int i = 0; i < n; i++) soma_sq += (valores[i] - media) * (valores[i] - media);
        r = soma_sq / (n - 1);
        if (strcmp(comando, "desvio") == 0) r = sqrt(r);
    }
    else if (strcmp(comando, "moda") == 0) {
        double moda_val = valores[0];
        int moda_cnt = 1;
        for (int i = 0; i < n; i++) {
            int cnt = 0;
            for (int j = 0; j < n; j++) if (valores[j] == valores[i]) cnt++;
            if (cnt > moda_cnt) { moda_cnt = cnt; moda_val = valores[i]; }
        }
        r = moda_val;
    }
    else {
        strcpy(resultado, "ERRO|Comandos: media mediana moda desvio variancia soma min max contagem");
        return 0;
    }

    if (r == (long long)r && fabs(r) < 1e15)
        snprintf(resultado, 256, "%lld", (long long)r);
    else
        snprintf(resultado, 256, "%.6f", r);
    return 1;
}

/* Conversao: "valor unidade_unidade" */
int calc_conversao(const char *expr, char *resultado) {
    if (_stricmp(expr, "listar") == 0) {
        strcpy(resultado,
            "\n  [Comprimento] km_m m_cm m_mm mi_km pol_cm pe_m"
            "\n  [Massa] kg_g g_mg t_kg lb_kg"
            "\n  [Temperatura] c_f f_c c_k k_c"
            "\n  [Dados] gb_mb mb_kb kb_b tb_gb");
        return 1;
    }

    double valor;
    char conv[20];
    if (sscanf(expr, "%lf %19s", &valor, conv) != 2) {
        strcpy(resultado, "ERRO|Use: valor conversao (ex: 100 km_m) ou 'listar'");
        return 0;
    }

    /* Converte conv para minusculo */
    for (int i = 0; conv[i]; i++) if (conv[i] >= 'A' && conv[i] <= 'Z') conv[i] += 32;

    double r;
    int especial = 0;

    /* Comprimento */
    if (strcmp(conv, "km_m") == 0)       r = valor * 1000;
    else if (strcmp(conv, "m_cm") == 0)  r = valor * 100;
    else if (strcmp(conv, "m_mm") == 0)  r = valor * 1000;
    else if (strcmp(conv, "cm_mm") == 0) r = valor * 10;
    else if (strcmp(conv, "mi_km") == 0) r = valor * 1.60934;
    else if (strcmp(conv, "pol_cm") == 0)r = valor * 2.54;
    else if (strcmp(conv, "pe_m") == 0)  r = valor * 0.3048;
    /* Massa */
    else if (strcmp(conv, "kg_g") == 0)  r = valor * 1000;
    else if (strcmp(conv, "g_mg") == 0)  r = valor * 1000;
    else if (strcmp(conv, "t_kg") == 0)  r = valor * 1000;
    else if (strcmp(conv, "lb_kg") == 0) r = valor * 0.453592;
    /* Temperatura */
    else if (strcmp(conv, "c_f") == 0)  { r = valor * 9.0/5.0 + 32; especial = 1; }
    else if (strcmp(conv, "f_c") == 0)  { r = (valor - 32) * 5.0/9.0; especial = 1; }
    else if (strcmp(conv, "c_k") == 0)  { r = valor + 273.15; especial = 1; }
    else if (strcmp(conv, "k_c") == 0)  { r = valor - 273.15; especial = 1; }
    /* Dados */
    else if (strcmp(conv, "gb_mb") == 0) r = valor * 1024;
    else if (strcmp(conv, "mb_kb") == 0) r = valor * 1024;
    else if (strcmp(conv, "kb_b") == 0)  r = valor * 1024;
    else if (strcmp(conv, "tb_gb") == 0) r = valor * 1024;
    else {
        snprintf(resultado, 256, "ERRO|Conversao '%s' desconhecida. Digite 'listar'", conv);
        return 0;
    }

    /* Extrai unidades do nome da conversao */
    char de[10] = {0}, para[10] = {0};
    char *sep = strchr(conv, '_');
    if (sep) {
        int pos = (int)(sep - conv);
        strncpy(de, conv, pos); de[pos] = '\0';
        strcpy(para, sep + 1);
        /* Maiusculo */
        for (int i = 0; de[i]; i++) if (de[i] >= 'a' && de[i] <= 'z') de[i] -= 32;
        for (int i = 0; para[i]; i++) if (para[i] >= 'a' && para[i] <= 'z') para[i] -= 32;
    }

    snprintf(resultado, 256, "%.6g %s = %.6g %s", valor, de, r, para);
    return 1;
}

/* ============================================================
   OBTER IP LOCAL
   ============================================================ */
void obter_ip_local(char *ip_out, int tam) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { strncpy(ip_out, "127.0.0.1", tam); return; }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);

    if (connect(s, (struct sockaddr*)&dest, sizeof(dest)) == 0) {
        struct sockaddr_in local;
        int len = sizeof(local);
        getsockname(s, (struct sockaddr*)&local, &len);
        inet_ntop(AF_INET, &local.sin_addr, ip_out, tam);
    } else {
        strncpy(ip_out, "127.0.0.1", tam);
    }
    closesocket(s);
}

/* ============================================================
   HANDLER DE CLIENTE (thread)
   ============================================================ */
DWORD WINAPI tratar_cliente(LPVOID param) {
    int idx = (int)(intptr_t)param;
    Sessao *sess = &sessoes[idx];
    SOCKET sock = sess->sock;
    char buf[BUFFER_TAM], resto[BUFFER_TAM];
    int resto_len = 0;
    char log_buf[512];

    snprintf(log_buf, sizeof(log_buf), "Nova conexao de %s", sess->ip);
    registrar_log("INFO", log_buf);

    EnterCriticalSection(&cs_estado);
    total_requisicoes++;
    LeaveCriticalSection(&cs_estado);

    /* Boas-vindas */
    enviar_msg(sock, "BEMVINDO", "Servidor de Calculo Distribuido SPD|1.0");

    /* --- Autenticacao --- */
    int autenticado = 0, tentativas = 0;

    while (!autenticado && tentativas < 5) {
        int n = receber_linha(sock, buf, sizeof(buf), resto, &resto_len);
        if (n <= 0) goto fim;
        tentativas++;

        /* LOGIN|usuario|senha  ou  REGISTRO|usuario|senha */
        char tipo[20] = {0}, nome[50] = {0}, senha[50] = {0};
        char *p1 = strchr(buf, '|');
        if (!p1) { enviar_msg(sock, "ERRO", "Formato invalido"); continue; }
        *p1 = '\0'; strcpy(tipo, buf);
        char *p2 = strchr(p1 + 1, '|');
        if (!p2) { enviar_msg(sock, "ERRO", "Formato invalido"); continue; }
        *p2 = '\0';
        strncpy(nome, p1 + 1, sizeof(nome) - 1);
        strncpy(senha, p2 + 1, sizeof(senha) - 1);

        if (_stricmp(tipo, "LOGIN") == 0) {
            const char *papel = autenticar(nome, senha);
            if (papel) {
                autenticado = 1;
                strncpy(sess->usuario, nome, sizeof(sess->usuario) - 1);
                strncpy(sess->papel, papel, sizeof(sess->papel) - 1);
                strcpy(sess->modo, "basico");

                char resp[256];
                snprintf(resp, sizeof(resp), "%s|%s|Bem-vindo, %s! Modo atual: basico", nome, papel, nome);
                enviar_msg(sock, "LOGIN_OK", resp);

                snprintf(log_buf, sizeof(log_buf), "Login: %s (%s) de %s", nome, papel, sess->ip);
                registrar_log("ACESSO", log_buf);
            } else {
                enviar_msg(sock, "ERRO", "Credenciais invalidas");
            }
        }
        else if (_stricmp(tipo, "REGISTRO") == 0) {
            if (strlen(nome) < 3 || strlen(senha) < 4) {
                enviar_msg(sock, "ERRO", "Usuario (min 3) e senha (min 4) muito curtos");
                continue;
            }
            char msg_r[100];
            int ok = registrar_usuario(nome, senha, msg_r);
            if (ok) {
                enviar_msg(sock, "INFO", "Registrado com sucesso. Faca login agora.");
                snprintf(log_buf, sizeof(log_buf), "Novo usuario: %s de %s", nome, sess->ip);
                registrar_log("INFO", log_buf);
            } else {
                enviar_msg(sock, "ERRO", msg_r);
            }
        }
        else {
            enviar_msg(sock, "ERRO", "Envie LOGIN ou REGISTRO primeiro");
        }
    }

    if (!autenticado) {
        enviar_msg(sock, "ERRO", "Muitas tentativas. Desconectando.");
        goto fim;
    }

    /* --- Loop principal --- */
    while (1) {
        int n = receber_linha(sock, buf, sizeof(buf), resto, &resto_len);
        if (n <= 0) break;

        EnterCriticalSection(&cs_estado);
        total_requisicoes++;
        LeaveCriticalSection(&cs_estado);

        /* Parse: TIPO|conteudo */
        char tipo[20] = {0}, conteudo[BUFFER_TAM] = {0};
        char *sep = strchr(buf, '|');
        if (sep) {
            *sep = '\0';
            strncpy(tipo, buf, sizeof(tipo) - 1);
            strncpy(conteudo, sep + 1, sizeof(conteudo) - 1);
        } else {
            strncpy(tipo, buf, sizeof(tipo) - 1);
        }

        /* --- COMANDOS --- */
        if (_stricmp(tipo, "CMD") == 0) {
            char cmd[20] = {0}, args[BUFFER_TAM] = {0};
            sscanf(conteudo, "%19s %[^\n]", cmd, args);

            if (_stricmp(cmd, "sair") == 0) {
                enviar_msg(sock, "INFO", "Ate logo!");
                break;
            }
            else if (_stricmp(cmd, "ajuda") == 0) {
                char ajuda[2048];
                int off = 0;
                off += snprintf(ajuda + off, sizeof(ajuda) - off,
                    "\n=== COMANDOS ===\n"
                    "  modo <nome>     - Trocar modo (basico, cientifico, estatistico, conversao)\n"
                    "  historico       - Ver historico\n"
                    "  limpar          - Limpar historico\n"
                    "  status          - Estatisticas do servidor\n"
                    "  ajuda           - Esta mensagem\n"
                    "  sair            - Desconectar\n");
                if (strcmp(sess->papel, "admin") == 0) {
                    off += snprintf(ajuda + off, sizeof(ajuda) - off,
                        "\n  --- ADMIN ---\n"
                        "  listar_usuarios - Listar usuarios\n"
                        "  sessoes         - Sessoes ativas\n"
                        "  broadcast <msg> - Mensagem global\n"
                        "  kick <usuario>  - Desconectar usuario\n");
                }
                enviar_msg(sock, "AJUDA", ajuda);
            }
            else if (_stricmp(cmd, "modo") == 0) {
                if (_stricmp(args, "basico") == 0 || _stricmp(args, "cientifico") == 0 ||
                    _stricmp(args, "estatistico") == 0 || _stricmp(args, "conversao") == 0) {
                    strcpy(sess->modo, args);
                    /* minusculo */
                    for (int i = 0; sess->modo[i]; i++)
                        if (sess->modo[i] >= 'A' && sess->modo[i] <= 'Z') sess->modo[i] += 32;
                    char resp[256];
                    snprintf(resp, sizeof(resp), "%s", sess->modo);
                    enviar_msg(sock, "MODO", resp);
                    snprintf(log_buf, sizeof(log_buf), "%s trocou para modo: %s", sess->usuario, sess->modo);
                    registrar_log("INFO", log_buf);
                } else {
                    enviar_msg(sock, "ERRO", "Modos: basico, cientifico, estatistico, conversao");
                }
            }
            else if (_stricmp(cmd, "historico") == 0) {
                char hist[BUFFER_TAM] = {0};
                int hoff = 0;
                int start = sess->hist_count > 20 ? sess->hist_count - 20 : 0;
                hoff += snprintf(hist + hoff, sizeof(hist) - hoff, "%d registros\n", sess->hist_count);
                for (int i = start; i < sess->hist_count; i++) {
                    RegistroCalculo *r = &sess->historico[i];
                    hoff += snprintf(hist + hoff, sizeof(hist) - hoff,
                        "  %s [%s] %s -> %s %s\n",
                        r->hora, r->modo, r->expressao, r->resultado,
                        r->sucesso ? "" : "(ERRO)");
                    if (hoff >= (int)sizeof(hist) - 100) break;
                }
                enviar_msg(sock, "HISTORICO", hist);
            }
            else if (_stricmp(cmd, "limpar") == 0) {
                sess->hist_count = 0;
                enviar_msg(sock, "INFO", "Historico limpo");
            }
            else if (_stricmp(cmd, "status") == 0) {
                EnterCriticalSection(&cs_estado);
                time_t agora = time(NULL);
                int uptime = (int)(agora - inicio_servidor);
                int horas = uptime / 3600, minutos = (uptime % 3600) / 60, segs = uptime % 60;
                int ativos = 0;
                EnterCriticalSection(&cs_sessoes);
                for (int i = 0; i < MAX_CLIENTES; i++) if (sessoes[i].ativo) ativos++;
                LeaveCriticalSection(&cs_sessoes);
                char resp[512];
                snprintf(resp, sizeof(resp),
                    "Uptime: %02d:%02d:%02d | Clientes: %d | Requisicoes: %d | Calculos: %d | Erros: %d",
                    horas, minutos, segs, ativos, total_requisicoes, total_calculos, total_erros);
                LeaveCriticalSection(&cs_estado);
                enviar_msg(sock, "STATUS", resp);
            }
            else if (_stricmp(cmd, "listar_usuarios") == 0 && strcmp(sess->papel, "admin") == 0) {
                char lista[2048] = {0};
                int loff = 0;
                EnterCriticalSection(&cs_usuarios);
                for (int i = 0; i < num_usuarios; i++) {
                    loff += snprintf(lista + loff, sizeof(lista) - loff,
                        "  %s (%s)\n", usuarios[i].nome, usuarios[i].papel);
                }
                LeaveCriticalSection(&cs_usuarios);
                enviar_msg(sock, "ADMIN", lista);
                snprintf(log_buf, sizeof(log_buf), "%s listou usuarios", sess->usuario);
                registrar_log("ADMIN", log_buf);
            }
            else if (_stricmp(cmd, "sessoes") == 0 && strcmp(sess->papel, "admin") == 0) {
                char lista[2048] = {0};
                int loff = 0;
                EnterCriticalSection(&cs_sessoes);
                for (int i = 0; i < MAX_CLIENTES; i++) {
                    if (sessoes[i].ativo) {
                        loff += snprintf(lista + loff, sizeof(lista) - loff,
                            "  %s | %s (%s) | modo: %s | calculos: %d\n",
                            sessoes[i].ip, sessoes[i].usuario, sessoes[i].papel,
                            sessoes[i].modo, sessoes[i].hist_count);
                    }
                }
                LeaveCriticalSection(&cs_sessoes);
                enviar_msg(sock, "ADMIN", lista);
            }
            else if (_stricmp(cmd, "broadcast") == 0 && strcmp(sess->papel, "admin") == 0) {
                if (strlen(args) == 0) {
                    enviar_msg(sock, "ERRO", "Use: broadcast <mensagem>");
                } else {
                    char bc[512];
                    snprintf(bc, sizeof(bc), "%s|%s", sess->usuario, args);
                    EnterCriticalSection(&cs_sessoes);
                    for (int i = 0; i < MAX_CLIENTES; i++) {
                        if (sessoes[i].ativo) {
                            enviar_msg(sessoes[i].sock, "BROADCAST", bc);
                        }
                    }
                    LeaveCriticalSection(&cs_sessoes);
                    snprintf(log_buf, sizeof(log_buf), "%s broadcast: %s", sess->usuario, args);
                    registrar_log("ADMIN", log_buf);
                }
            }
            else if (_stricmp(cmd, "kick") == 0 && strcmp(sess->papel, "admin") == 0) {
                if (strlen(args) == 0) {
                    enviar_msg(sock, "ERRO", "Use: kick <usuario>");
                } else {
                    int kicked = 0;
                    EnterCriticalSection(&cs_sessoes);
                    for (int i = 0; i < MAX_CLIENTES; i++) {
                        if (sessoes[i].ativo && i != idx && strcmp(sessoes[i].usuario, args) == 0) {
                            enviar_msg(sessoes[i].sock, "INFO", "Desconectado pelo admin.");
                            closesocket(sessoes[i].sock);
                            sessoes[i].ativo = 0;
                            kicked = 1;
                        }
                    }
                    LeaveCriticalSection(&cs_sessoes);
                    if (kicked) {
                        char resp[100];
                        snprintf(resp, sizeof(resp), "%s desconectado", args);
                        enviar_msg(sock, "INFO", resp);
                        snprintf(log_buf, sizeof(log_buf), "%s kick: %s", sess->usuario, args);
                        registrar_log("ADMIN", log_buf);
                    } else {
                        enviar_msg(sock, "ERRO", "Usuario nao encontrado nas sessoes ativas");
                    }
                }
            }
            else if ((_stricmp(cmd, "listar_usuarios") == 0 || _stricmp(cmd, "sessoes") == 0 ||
                      _stricmp(cmd, "broadcast") == 0 || _stricmp(cmd, "kick") == 0) &&
                     strcmp(sess->papel, "admin") != 0) {
                enviar_msg(sock, "ERRO", "Permissao negada. Comando de admin.");
            }
            else {
                enviar_msg(sock, "ERRO", "Comando desconhecido. Digite 'ajuda'.");
            }
        }
        /* --- CALCULO --- */
        else if (_stricmp(tipo, "CALC") == 0) {
            EnterCriticalSection(&cs_estado);
            total_calculos++;
            LeaveCriticalSection(&cs_estado);

            char resultado[256] = {0};
            int ok;

            if (strcmp(sess->modo, "basico") == 0)           ok = calc_basico(conteudo, resultado);
            else if (strcmp(sess->modo, "cientifico") == 0)   ok = calc_cientifico(conteudo, resultado);
            else if (strcmp(sess->modo, "estatistico") == 0)  ok = calc_estatistico(conteudo, resultado);
            else if (strcmp(sess->modo, "conversao") == 0)    ok = calc_conversao(conteudo, resultado);
            else { ok = 0; strcpy(resultado, "ERRO|Modo invalido"); }

            /* Registra historico */
            if (sess->hist_count < MAX_HISTORICO) {
                RegistroCalculo *reg = &sess->historico[sess->hist_count++];
                time_t agora = time(NULL);
                struct tm *t = localtime(&agora);
                strftime(reg->hora, sizeof(reg->hora), "%H:%M:%S", t);
                strncpy(reg->modo, sess->modo, sizeof(reg->modo) - 1);
                strncpy(reg->expressao, conteudo, sizeof(reg->expressao) - 1);
                /* Se erro, resultado comeca com "ERRO|" */
                if (strncmp(resultado, "ERRO|", 5) == 0) {
                    strncpy(reg->resultado, resultado + 5, sizeof(reg->resultado) - 1);
                    reg->sucesso = 0;
                } else {
                    strncpy(reg->resultado, resultado, sizeof(reg->resultado) - 1);
                    reg->sucesso = 1;
                }
            }

            if (strncmp(resultado, "ERRO|", 5) == 0) {
                EnterCriticalSection(&cs_estado);
                total_erros++;
                LeaveCriticalSection(&cs_estado);
                enviar_msg(sock, "ERRO_CALC", resultado + 5);
                snprintf(log_buf, sizeof(log_buf), "[%s] Erro: %s -> %s", sess->usuario, conteudo, resultado + 5);
                registrar_log("AVISO", log_buf);
            } else {
                char resp[512];
                snprintf(resp, sizeof(resp), "%s|%s", conteudo, resultado);
                enviar_msg(sock, "RESULTADO", resp);
                snprintf(log_buf, sizeof(log_buf), "[%s] %s = %s", sess->usuario, conteudo, resultado);
                registrar_log("INFO", log_buf);
            }
        }
        else {
            enviar_msg(sock, "ERRO", "Tipo desconhecido");
        }
    }

fim:
    closesocket(sock);
    EnterCriticalSection(&cs_sessoes);
    sess->ativo = 0;
    LeaveCriticalSection(&cs_sessoes);

    snprintf(log_buf, sizeof(log_buf), "Desconectado: %s (%s)", sess->usuario[0] ? sess->usuario : "?", sess->ip);
    registrar_log("INFO", log_buf);
    return 0;
}

/* ============================================================
   MAIN
   ============================================================ */
int main(void) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("Erro ao inicializar Winsock\n");
        return 1;
    }

    InitializeCriticalSection(&cs_usuarios);
    InitializeCriticalSection(&cs_sessoes);
    InitializeCriticalSection(&cs_estado);
    InitializeCriticalSection(&cs_log);
    inicializar_usuarios();
    memset(sessoes, 0, sizeof(sessoes));
    inicio_servidor = time(NULL);

    /* Descobre IP local */
    char ip_local[64];
    obter_ip_local(ip_local, sizeof(ip_local));

    /* Banner */
    printf("\n");
    printf("  ____  ____  ____    ____\n");
    printf(" / ___||  _ \\|  _ \\  / ___|  ___ _ ____   _____ _ __\n");
    printf(" \\___ \\| |_) | | | | \\___ \\ / _ \\ '__\\ \\ / / _ \\ '__|\n");
    printf("  ___) |  __/| |_| |  ___) |  __/ |   \\ V /  __/ |\n");
    printf(" |____/|_|   |____/  |____/ \\___|_|    \\_/ \\___|_|\n");
    printf("\n");
    printf(" Servidor de Calculo Distribuido\n");
    printf(" Porta %d | Max %d conexoes\n", PORTA, MAX_CLIENTES);
    printf(" Usuarios padrao: admin/admin123 | aluno/spd2026\n\n");
    printf(" IP do servidor: %s\n", ip_local);
    printf(" Para conectar de outro PC:\n");
    printf("   cliente.exe %s\n\n", ip_local);

    /* Cria socket */
    SOCKET servidor = socket(AF_INET, SOCK_STREAM, 0);
    if (servidor == INVALID_SOCKET) {
        printf("Erro ao criar socket: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in endereco;
    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = INADDR_ANY;
    endereco.sin_port = htons(PORTA);

    if (bind(servidor, (struct sockaddr*)&endereco, sizeof(endereco)) == SOCKET_ERROR) {
        printf("Erro no bind: %d\n", WSAGetLastError());
        closesocket(servidor);
        WSACleanup();
        return 1;
    }

    if (listen(servidor, MAX_CLIENTES) == SOCKET_ERROR) {
        printf("Erro no listen: %d\n", WSAGetLastError());
        closesocket(servidor);
        WSACleanup();
        return 1;
    }

    registrar_log("INFO", "Servidor iniciado. Aguardando conexoes...");

    while (1) {
        struct sockaddr_in cli_addr;
        int cli_len = sizeof(cli_addr);
        SOCKET cli_sock = accept(servidor, (struct sockaddr*)&cli_addr, &cli_len);
        if (cli_sock == INVALID_SOCKET) continue;

        /* Encontra slot livre */
        int slot = -1;
        EnterCriticalSection(&cs_sessoes);
        for (int i = 0; i < MAX_CLIENTES; i++) {
            if (!sessoes[i].ativo) {
                slot = i;
                memset(&sessoes[i], 0, sizeof(Sessao));
                sessoes[i].sock = cli_sock;
                sessoes[i].ativo = 1;
                inet_ntop(AF_INET, &cli_addr.sin_addr, sessoes[i].ip, sizeof(sessoes[i].ip));
                char porta_str[10];
                snprintf(porta_str, sizeof(porta_str), ":%d", ntohs(cli_addr.sin_port));
                strcat(sessoes[i].ip, porta_str);
                break;
            }
        }
        LeaveCriticalSection(&cs_sessoes);

        if (slot == -1) {
            const char *msg = "ERRO|Servidor lotado\n";
            send(cli_sock, msg, (int)strlen(msg), 0);
            closesocket(cli_sock);
            registrar_log("AVISO", "Conexao recusada: servidor lotado");
            continue;
        }

        HANDLE thread = CreateThread(NULL, 0, tratar_cliente, (LPVOID)(intptr_t)slot, 0, NULL);
        if (thread) CloseHandle(thread);
    }

    closesocket(servidor);
    WSACleanup();
    return 0;
}
