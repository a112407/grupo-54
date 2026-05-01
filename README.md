# grupo-54

Projeto de Sistemas Operativos com dois executáveis:

- `controller`: recebe pedidos, gere a fila de comandos e controla o paralelismo
- `runner`: submete comandos, pede consultas ao estado e termina o controlador

## Autores

- Diogo Oliveira da Silva a111493
- Joana Catarina Fernandes Rodrigues a112407
- Luís Ricardo Rodrigues Gonçalves a111168

## O que o projeto faz

O sistema usa FIFOs para comunicar entre processos. O `runner` envia mensagens ao `controller` para:

- submeter comandos com um utilizador associado (`-e`)
- consultar o estado atual da fila (`-c`)
- pedir o encerramento do sistema (`-s`)

O `controller` guarda os comandos em espera, liberta execuções até ao limite de paralelismo configurado e escreve o histórico em `tmp/history.log`.

## Estrutura

- `src/controller.c` - lógica do controlador, fila, escalonamento e logs
- `src/runner.c` - interface de cliente e execução dos comandos
- `include/common.h` - estruturas e constantes partilhadas
- `scripts/` - scripts de teste e validação
- `tmp/` - FIFOs temporários e ficheiro de histórico

## Compilação

```bash
make
```

Isto gera os executáveis em `bin/controller` e `bin/runner`.

Para limpar artefactos de compilação e ficheiros temporários:

```bash
make clean
```

## Execução

### Arrancar o controller

```bash
./bin/controller [max_parallel] [fifo|rr]
```

- `max_parallel` é opcional e define o número máximo de comandos em execução ao mesmo tempo
- `fifo` é a política por omissão
- `rr` ativa round-robin por utilizador

Exemplos:

```bash
./bin/controller
./bin/controller 2 fifo
./bin/controller 4 rr
```

### Submeter comandos

```bash
./bin/runner -e <user> <cmd> [args...]
```

Exemplos:

```bash
./bin/runner -e alice "echo ola"
./bin/runner -e bob "ls -l"
./bin/runner -e carol "cat input.txt | grep teste > saida.txt"
```

O comando é passado como string ao `runner`, que suporta pipelines simples e redirecionamentos `>`, `<` e `2>`.

### Consultar o estado

```bash
./bin/runner -c
```

Mostra os comandos em execução e os comandos agendados.

### Encerrar o sistema

```bash
./bin/runner -s
```

O `runner` envia o pedido de shutdown e espera pela confirmação do `controller`.

## Testes

Os scripts em `scripts/` exercitam os cenários principais:

- `scripts/test_basic.sh` - testes básicos de execução, query e shutdown
- `scripts/test_parallel.sh` - valida o paralelismo configurado
- `scripts/test_compare.sh` - compara FIFO com round-robin

Exemplo de execução:

```bash
./scripts/test_basic.sh
```

## Saídas e ficheiros temporários

- `tmp/controller.pipe` - FIFO do controlador
- `tmp/runner_<pid>.pipe` - FIFO privado de cada runner
- `tmp/history.log` - histórico de execuções concluídas

## Notas

- O projeto foi pensado para ambiente Linux.
- As mensagens entre processos usam estruturas em memória, por isso os executáveis devem ser compilados no mesmo ambiente.
- Se o `controller` terminar de forma anormal, pode ser útil limpar `tmp/` antes de voltar a correr o sistema.
