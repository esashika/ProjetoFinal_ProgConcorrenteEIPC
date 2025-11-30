# Projeto Final - Programação Concorrente e IPC 🍕

Simulação completa de uma pizzaria concorrente desenvolvida para a disciplina de Sistemas Operacionais / Programação Concorrente. O projeto demonstra threads, processos, sincronização e comunicação entre processos em um fluxo que vai do pedido à entrega.

## 👥 Equipe

- Chrystian Caldas
- Edward Junior
- Fabio Braz
- Isabella Cabral
- Rhedson Esashika

## 🧱 Estrutura do Repositório

```
Projeto Final - Programação Concorrente e IPC/
├── Makefile
├── README.MD
└── src/
    └── pizzaria.c
```

## 🛠 Tecnologias e Conceitos Utilizados

| Conceito                      | Implementação no Código                                                          |
| :---------------------------- | :------------------------------------------------------------------------------- |
| **Threads POSIX**             | `pthread_create` para criar Garçons e Pizzaiolos.                                |
| **Processos**                 | `fork()` cria o processo de Delivery; `waitpid()` aguarda finalização.           |
| **Exclusão Mútua**            | `pthread_mutex_t` protege a fila de pedidos e o `stdout` (logs).                 |
| **Sincronização Condicional** | `pthread_cond_t` (cheio/vazio) coordena a fila entre Garçons e Pizzaiolos.       |
| **Controle do Forno**         | Mutex + condition variable limitando quantas pizzas podem assar simultaneamente. |
| **IPC (Comunicação)**         | `pipe()` (unnamed pipe) envia pedidos prontos ao processo Delivery.              |

## 📋 Descrição do Fluxo

1. **Garçons (threads produtoras)** recebem pedidos e os colocam em uma fila circular protegida por mutex/condvars.
2. **Pizzaiolos (threads consumidoras)** retiram os pedidos, montam as pizzas e disputam o forno (recurso limitado).
3. **Forno** é controlado por um contador protegido (mutex + condvar) que limita o número de pizzas simultâneas.
4. **Delivery (processo filho)** lê IDs de pedidos pelo pipe e simula a entrega.

## 🚀 Como Executar

### Pré-requisitos

- Compilador GCC
- Ambiente Linux/Unix ou macOS (bibliotecas POSIX)

### Compilação com Make

```bash
make
```

### Execução

```bash
make run
```

### Compilação Manual

```bash
gcc src/pizzaria.c -o pizzaria -pthread
```

O binário gerado via `make` fica em `build/pizzaria`.

## ⚙️ Fluxo de Execução (Resumo)

O programa cria o pipe e o processo Delivery (fork). O processo pai inicializa as threads de Garçons e Pizzaiolos, que sincronizam a fila de pedidos e disputam o forno limitado. A cada pedido finalizado, o ID segue para o processo Delivery via pipe, encerrando somente após a confirmação de todos os pedidos.

---
