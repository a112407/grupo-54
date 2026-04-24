#!/bin/bash
# test_parallel.sh — testa diferentes niveis de paralelismo
cd "$(dirname "$0")/.."

# limpeza defensiva
pkill -9 -f "bin/controller" 2>/dev/null
pkill -9 -f "bin/runner" 2>/dev/null
rm -f tmp/*
sleep 0.5

echo "=========================================="
echo "  TESTE DE PARALELISMO"
echo "=========================================="
echo "  Carga: 8 comandos 'sleep 1' de varios users"
echo "=========================================="

run_test() {
    local parallel=$1

    echo
    echo "─── parallel=$parallel ───"

    # limpar antes de cada teste
    pkill -9 -f "bin/controller" 2>/dev/null
    pkill -9 -f "bin/runner" 2>/dev/null
    rm -f tmp/*
    sleep 0.3

    # arrancar controller com o nivel de paralelismo
    ./bin/controller "$parallel" fifo > /dev/null 2>&1 &
    local CPID=$!
    sleep 0.5

    # marcar tempo de inicio
    local START=$(date +%s)

    # submeter 8 comandos (4 users, 2 cmds cada)
    ./bin/runner -e alice "sleep 1" > /dev/null 2>&1 &
    ./bin/runner -e alice "sleep 1" > /dev/null 2>&1 &
    ./bin/runner -e bob   "sleep 1" > /dev/null 2>&1 &
    ./bin/runner -e bob   "sleep 1" > /dev/null 2>&1 &
    ./bin/runner -e carol "sleep 1" > /dev/null 2>&1 &
    ./bin/runner -e carol "sleep 1" > /dev/null 2>&1 &
    ./bin/runner -e dave  "sleep 1" > /dev/null 2>&1 &
    ./bin/runner -e dave  "sleep 1" > /dev/null 2>&1 &

    echo "  (a aguardar execucao...)"

    # esperar pelos runners (nao pelo controller)
    for pid in $(jobs -p); do
        if [ "$pid" != "$CPID" ]; then
            wait "$pid" 2>/dev/null
        fi
    done

    # marcar tempo final
    local END=$(date +%s)
    local TOTAL=$((END - START))

    # matar controller
    sleep 0.3
    kill -9 "$CPID" 2>/dev/null
    wait "$CPID" 2>/dev/null

    # resultados
    local NUM_CMDS=$(wc -l < tmp/history.log 2>/dev/null || echo 0)
    echo "  Comandos executados: $NUM_CMDS"
    echo "  Tempo total:         ${TOTAL}s"

    # calcular throughput (cmds/segundo)
    if [ "$TOTAL" -gt 0 ]; then
        local THROUGHPUT=$(awk "BEGIN {printf \"%.2f\", $NUM_CMDS / $TOTAL}")
        echo "  Throughput:          $THROUGHPUT cmds/s"
    fi
}

run_test 1
run_test 2
run_test 4

# limpeza final
pkill -9 -f "bin/controller" 2>/dev/null
pkill -9 -f "bin/runner" 2>/dev/null
rm -f tmp/*

echo
echo "=========================================="
echo "  ANALISE:"
echo "  - parallel=1: todos em sequencia (~8s)"
echo "  - parallel=2: 2 em paralelo     (~4s)"
echo "  - parallel=4: 4 em paralelo     (~2s)"
echo "=========================================="