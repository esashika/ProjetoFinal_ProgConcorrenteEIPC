/*
 * pizzaria_fixed.c
 * Versão melhorada do exercício "Pizzaria" com:
 *  - tratamento de retornos de chamadas
 *  - contagem correta do total de pedidos
 *  - proteção de I/O (ignorar SIGPIPE e checar write/read)
 *  - finalização limpa (semáforos, condvars, mutexes)
 *
 * Compilar:
 *   gcc pizzaria_fixed.c -o pizzaria_fixed -pthread
 *
 * Executar:
 *   ./pizzaria_fixed
 *
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <time.h>

#define COLOR_RESET "\033[0m"
#define COLOR_INFO "\033[1;37m"
#define COLOR_GARCOM "\033[1;34m"
#define COLOR_PIZZAIOLO "\033[1;32m"
#define COLOR_DELIVERY "\033[1;35m"
#define COLOR_MAIN "\033[1;33m"

/* Configurações (ajuste conforme necessário) */
#define NUM_GARCONS 3
#define NUM_PIZZAIOLOS 2
#define PEDIDOS_POR_GARCOM 5
#define FILA_CAPACITY 8

/* Estruturas para fila circular de pedidos */
int fila[FILA_CAPACITY];
int head = 0, tail = 0, count = 0;

/* Contadores globais */
int total_pedidos = NUM_GARCONS * PEDIDOS_POR_GARCOM;
int pedidos_processados = 0;

/* Sincronização */
pthread_mutex_t mutex_fila = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_cheio = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_vazio = PTHREAD_COND_INITIALIZER;

/* Controle do forno usando mutex+cond (evita semáforos deprecated no macOS) */
pthread_mutex_t forno_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t forno_cond = PTHREAD_COND_INITIALIZER;
int forno_disponivel = NUM_PIZZAIOLOS;

/* Mutex para I/O (imprimir linhas completas) */
pthread_mutex_t io_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Pipe para comunicação Cozinha -> Delivery (processo filho) */
int pipe_fd[2] = {-1, -1};

/* Função utilitária para logging thread-safe */
void log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&io_mutex);
    vprintf(fmt, ap);
    fflush(stdout);
    pthread_mutex_unlock(&io_mutex);
    va_end(ap);
}

void log_printf_color(const char *color, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&io_mutex);
    if (color != NULL) fputs(color, stdout);
    vprintf(fmt, ap);
    if (color != NULL) fputs(COLOR_RESET, stdout);
    fflush(stdout);
    pthread_mutex_unlock(&io_mutex);
    va_end(ap);
}

#define LOG_INFO(...) log_printf_color(COLOR_INFO, __VA_ARGS__)
#define LOG_GARCOM(...) log_printf_color(COLOR_GARCOM, __VA_ARGS__)
#define LOG_PIZZAIOLO(...) log_printf_color(COLOR_PIZZAIOLO, __VA_ARGS__)
#define LOG_DELIVERY(...) log_printf_color(COLOR_DELIVERY, __VA_ARGS__)
#define LOG_MAIN(...) log_printf_color(COLOR_MAIN, __VA_ARGS__)

/* push na fila (assume mutex preso) */
int fila_push(int pedido) {
    if (count >= FILA_CAPACITY) return -1;
    fila[tail] = pedido;
    tail = (tail + 1) % FILA_CAPACITY;
    count++;
    return 0;
}

/* pop da fila (assume mutex preso) */
int fila_pop(int *pedido) {
    if (count <= 0) return -1;
    *pedido = fila[head];
    head = (head + 1) % FILA_CAPACITY;
    count--;
    return 0;
}

void forno_acquire(void);
void forno_release(void);

/* Garçom - produtor */
void* garcom_thread(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < PEDIDOS_POR_GARCOM; i++) {
        int pedido_id = (int)(id * 1000 + i + 1); // id único por garçom+seq

        /* colocar pedido na fila (bloqueia se cheia) */
        if (pthread_mutex_lock(&mutex_fila) != 0) {
            perror("pthread_mutex_lock");
            break;
        }
        while (count >= FILA_CAPACITY) {
            // fila cheia -> esperar espaço
            pthread_cond_wait(&cond_cheio, &mutex_fila);
        }
        if (fila_push(pedido_id) != 0) {
            // não deveria acontecer
            LOG_GARCOM("[GARCOM %ld] ERRO: não conseguiu enfileirar pedido %d\n", id, pedido_id);
        } else {
            LOG_GARCOM("[GARCOM %ld] ADICIONOU pedido %d (Fila=%d)\n", id, pedido_id, count);
        }
        // sinaliza consumidores
        pthread_cond_signal(&cond_vazio);
        if (pthread_mutex_unlock(&mutex_fila) != 0) {
            perror("pthread_mutex_unlock");
            break;
        }
        // simula tempo entre pedidos
        usleep(100000 + (rand() % 200000)); // 100-300 ms
    }
    LOG_GARCOM("[GARCOM %ld] terminou de gerar pedidos.\n", id);
    return NULL;
}

/* Pizzaiolo - consumidor/ produtor para forno e envio para delivery */
void* pizzaiolo_thread(void *arg) {
    long id = (long)arg;
    for (;;) {
        int pedido = 0;
        /* pega um pedido da fila */
        if (pthread_mutex_lock(&mutex_fila) != 0) {
            perror("pthread_mutex_lock");
            break;
        }

        // Se todos os pedidos foram processados e fila vazia, sai
        if (pedidos_processados >= total_pedidos && count == 0) {
            pthread_mutex_unlock(&mutex_fila);
            break;
        }

        while (count == 0) {
            // Se não há pedidos e todos já processados, sair
            if (pedidos_processados >= total_pedidos) {
                pthread_mutex_unlock(&mutex_fila);
                goto terminar_pizzaiolo;
            }
            pthread_cond_wait(&cond_vazio, &mutex_fila);
        }

        if (fila_pop(&pedido) == 0) {
            pedidos_processados++;
            LOG_PIZZAIOLO("  [PIZZAIOLO %ld] RETIROU pedido %d (restam fila=%d, total_processados=%d/%d)\n",
                          id, pedido, count, pedidos_processados, total_pedidos);
            // sinaliza produtores que há espaço
            pthread_cond_signal(&cond_cheio);
            if (pthread_mutex_unlock(&mutex_fila) != 0) {
                perror("pthread_mutex_unlock");
                break;
            }
        } else {
            if (pthread_mutex_unlock(&mutex_fila) != 0) perror("pthread_mutex_unlock");
            continue;
        }

        // montagem da pizza (região não crítica)
        LOG_PIZZAIOLO("  [PIZZAIOLO %ld] montando pizza %d...\n", id, pedido);
        usleep(200000 + (rand() % 300000)); // 200-500 ms montagem

        // usar forno (semáforo para limitar quantas pizzas podem assar simultaneamente)
        forno_acquire();
        LOG_PIZZAIOLO("  [PIZZAIOLO %ld] COLOCOU pedido %d no forno.\n", id, pedido);
        // simula tempo de forno
        usleep(300000 + (rand() % 400000)); // 300-700 ms
        forno_release();
        LOG_PIZZAIOLO("  [PIZZAIOLO %ld] RETIROU pedido %d do forno.\n", id, pedido);

        // enviar para processo delivery via pipe
        ssize_t w = write(pipe_fd[1], &pedido, sizeof(int));
        if (w != sizeof(int)) {
            // write pode falhar se pipe fechado -> reportar
            LOG_PIZZAIOLO("[PIZZAIOLO %ld] ERRO ao escrever no pipe (pedido %d): %s\n", id, pedido, strerror(errno));
        } else {
            LOG_PIZZAIOLO("[PIZZAIOLO %ld] enviou pedido %d para DELIVERY (pipe)\n", id, pedido);
        }
    }

terminar_pizzaiolo:
    LOG_PIZZAIOLO("[PIZZAIOLO %ld] finalizando (nenhum pedido restante).\n", id);
    return NULL;
}

/* Processo Delivery - filho que lê pedidos do pipe e "entrega" */
void delivery_process(void) {
    if (close(pipe_fd[1]) != 0) {
        // ignore error
    }
    LOG_DELIVERY("[DELIVERY] Processo DELIVERY iniciado (pid=%d). Aguardando pedidos...\n", getpid());
    int pedido;
    ssize_t r;
    while ((r = read(pipe_fd[0], &pedido, sizeof(int))) > 0) {
        if (r != sizeof(int)) {
            LOG_DELIVERY("[DELIVERY] Leu tamanho inesperado=%zd\n", r);
            continue;
        }
        LOG_DELIVERY("[DELIVERY] Recebi pedido %d. Entregando...\n", pedido);
        // simula entrega
        usleep(250000 + (rand() % 300000)); // 250-550 ms
        LOG_DELIVERY("[DELIVERY] Pedido %d entregue.\n", pedido);
    }
    if (r == 0) {
        LOG_DELIVERY("[DELIVERY] pipe fechado — nenhum pedido adicional. Encerrando.\n");
    } else {
        LOG_DELIVERY("[DELIVERY] Erro na leitura do pipe: %s\n", strerror(errno));
    }
    if (close(pipe_fd[0]) != 0) {
        // ignore
    }
    _exit(0);
}

/* Main */
int main(void) {
    srand((unsigned)time(NULL));
    LOG_INFO("=== PIZZARIA (versão FIXED) ===\n");
    LOG_INFO("Total pedidos a processar = %d\n", total_pedidos);

    /* Ignora SIGPIPE para evitar morte do processo ao escrever em pipe fechado */
    signal(SIGPIPE, SIG_IGN);

    /* cria pipe */
    if (pipe(pipe_fd) != 0) {
        perror("pipe");
        return 1;
    }

    /* cria processo delivery (filho) */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        // CHILD
        delivery_process();
        // nunca retorna
    }

    /* PAI segue: criar threads de garçons e pizzaiolos */
    pthread_t garcons[NUM_GARCONS];
    pthread_t pizzaiolos[NUM_PIZZAIOLOS];
    long ids_g[NUM_GARCONS];
    long ids_p[NUM_PIZZAIOLOS];

    // criar threads garcons
    for (long i = 0; i < NUM_GARCONS; i++) {
        ids_g[i] = i + 1;
        int rc = pthread_create(&garcons[i], NULL, garcom_thread, (void*)ids_g[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create garcom %ld falhou: %s\n", i+1, strerror(rc));
            // decidir continuar ou abortar; aqui abortamos
            exit(1);
        }
    }

    // criar threads pizzaiolos
    for (long i = 0; i < NUM_PIZZAIOLOS; i++) {
        ids_p[i] = i + 1;
        int rc = pthread_create(&pizzaiolos[i], NULL, pizzaiolo_thread, (void*)ids_p[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create pizzaiolo %ld falhou: %s\n", i+1, strerror(rc));
            exit(1);
        }
    }

    /* aguardar garçons terminarem (eles apenas produzem pedidos) */
    for (int i = 0; i < NUM_GARCONS; i++) {
        pthread_join(garcons[i], NULL);
    }
    LOG_MAIN("[MAIN] Todos os garçons terminaram. Aguardando processamento dos pedidos restantes...\n");

    /* aguardar pizzaiolos terminarem (les threads encerrarão quando todos pedidos processados) */
    for (int i = 0; i < NUM_PIZZAIOLOS; i++) {
        pthread_join(pizzaiolos[i], NULL);
    }

    /* fechando extremidade de escrita do pipe para sinalizar EOF ao filho delivery */
    if (close(pipe_fd[1]) != 0) {
        // ignore
    }

    /* esperar filho delivery terminar */
    int status = 0;
    waitpid(pid, &status, 0);
    LOG_MAIN("[MAIN] Processo DELIVERY finalizado. status=%d\n", WEXITSTATUS(status));

    /* destruir recursos */
    pthread_mutex_destroy(&mutex_fila);
    pthread_mutex_destroy(&io_mutex);
    pthread_cond_destroy(&cond_cheio);
    pthread_cond_destroy(&cond_vazio);
    pthread_mutex_destroy(&forno_mutex);
    pthread_cond_destroy(&forno_cond);

    LOG_INFO("=== FIM (Pizzaria) ===\n");
    return 0;
}
/* Controle de acesso ao forno */
void forno_acquire(void) {
    int rc = pthread_mutex_lock(&forno_mutex);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_lock forno falhou: %s\n", strerror(rc));
        return;
    }
    while (forno_disponivel == 0) {
        rc = pthread_cond_wait(&forno_cond, &forno_mutex);
        if (rc != 0) {
            fprintf(stderr, "pthread_cond_wait forno falhou: %s\n", strerror(rc));
            pthread_mutex_unlock(&forno_mutex);
            return;
        }
    }
    forno_disponivel--;
    pthread_mutex_unlock(&forno_mutex);
}

void forno_release(void) {
    int rc = pthread_mutex_lock(&forno_mutex);
    if (rc != 0) {
        fprintf(stderr, "pthread_mutex_lock forno falhou: %s\n", strerror(rc));
        return;
    }
    forno_disponivel++;
    rc = pthread_cond_signal(&forno_cond);
    if (rc != 0) {
        fprintf(stderr, "pthread_cond_signal forno falhou: %s\n", strerror(rc));
    }
    pthread_mutex_unlock(&forno_mutex);
}
