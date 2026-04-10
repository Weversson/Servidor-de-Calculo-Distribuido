# Servidor de Calculo Distribuido

Servidor TCP multi-threaded em C que recebe operacoes matematicas de multiplos clientes simultaneamente, processa e retorna o resultado. Cada cliente conectado e tratado em uma thread separada usando a API Windows (Winsock2 e CreateThread).

## Estrutura

O projeto possui dois arquivos executaveis:

- `servidor.exe` fica na maquina central e aguarda conexoes na porta 5000
- `cliente.exe` e distribuido para os usuarios que vao se conectar ao servidor

## Como executar

### Servidor

```
servidor.exe
```

Ao iniciar, o servidor exibe o IP da maquina na rede. Esse IP deve ser informado aos usuarios que vao se conectar.

### Cliente

```
cliente.exe
```

O cliente solicita o IP do servidor ao iniciar. Tambem aceita o IP como argumento:

```
cliente.exe 192.168.1.100
```

## Autenticacao

O servidor exige login para operar. Existem dois usuarios padroes:

| Usuario | Senha    | Papel   |
|---------|----------|---------|
| admin   | admin123 | admin   |
| aluno   | spd2026  | usuario |

Novos usuarios podem ser registrados pelo proprio cliente na tela inicial.

## Modos de calculo

O servidor opera em quatro modos, alternados pelo comando `modo <nome>`:

### Basico

Operacoes aritmeticas entre dois numeros.

```
5 + 3        -> 8
10 / 2       -> 5
2 ** 10      -> 1024
10 % 3       -> 1
15 // 4      -> 3
```

Operadores: `+` `-` `*` `/` `//` `**` `%`

### Cientifico

Funcoes matematicas e constantes.

```
raiz(144)    -> 12
sen(3.14)    -> 0.00159265
fat(5)       -> 120
pi           -> 3.14159265358979
euler        -> 2.71828182845905
```

Funcoes: `sen` `cos` `tan` `raiz` `log` `ln` `abs` `fat` `ceil` `floor` `graus` `rad`

### Estatistico

Operacoes sobre conjuntos de valores.

```
media 10 20 30       -> 20
mediana 1 3 5 7      -> 4
soma 1 2 3 4 5       -> 15
desvio 10 20 30      -> 10
```

Comandos: `media` `mediana` `moda` `desvio` `variancia` `soma` `min` `max` `contagem`

### Conversao

Conversao entre unidades. Digite `listar` para ver todas as opcoes.

```
100 km_m     -> 100 KM = 100000 M
100 c_f      -> 100 C = 212 F
0 c_k        -> 0 C = 273.15 K
1 gb_mb      -> 1 GB = 1024 MB
```

Categorias: comprimento, massa, temperatura e dados.

## Comandos

| Comando          | Descricao                        |
|------------------|----------------------------------|
| modo <nome>      | Trocar modo de calculo           |
| historico        | Ver historico da sessao          |
| limpar           | Limpar historico                 |
| status           | Estatisticas do servidor         |
| ajuda            | Lista de comandos                |
| sair             | Encerrar conexao                 |

### Comandos de administrador

| Comando             | Descricao                     |
|---------------------|-------------------------------|
| listar_usuarios     | Listar todos os usuarios      |
| sessoes             | Ver sessoes ativas            |
| broadcast <msg>     | Enviar mensagem a todos       |
| kick <usuario>      | Desconectar um usuario        |

## Protocolo de comunicacao

A comunicacao entre cliente e servidor usa TCP com mensagens em texto plano, delimitadas por `\n`. O formato de cada mensagem e:

```
TIPO|conteudo
```

Quebras de linha dentro do conteudo sao escapadas como `\n` literal para nao quebrar o protocolo.

Tipos de mensagem do cliente: `LOGIN`, `REGISTRO`, `CMD`, `CALC`

Tipos de mensagem do servidor: `BEMVINDO`, `LOGIN_OK`, `RESULTADO`, `ERRO`, `ERRO_CALC`, `MODO`, `AJUDA`, `HISTORICO`, `STATUS`, `ADMIN`, `BROADCAST`, `INFO`

## Detalhes tecnicos

Linguagem: C (compilado com GCC/MinGW)

Bibliotecas: Winsock2 (`ws2_32`), Windows API (`CreateThread`, `CriticalSection`)

O servidor cria uma thread para cada cliente conectado. O estado global (usuarios, sessoes, contadores) e protegido por `CRITICAL_SECTION` para acesso seguro entre threads. O servidor suporta ate 20 conexoes simultaneas.

O cliente usa leitura sincrona: envia a mensagem, espera a resposta e so entao exibe o prompt novamente. Broadcasts do administrador sao processados entre interacoes.

## Compilacao

Requer MinGW (GCC para Windows) instalado.

```
gcc servidor.c -o servidor.exe -lws2_32 -lm -O2
gcc cliente.c -o cliente.exe -lws2_32 -O2
```
