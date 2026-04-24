#!/bin/bash
# test_compare.sh — compara FIFO vs Round-Robin com carga injusta
cd "$(dirname "$0")/.."

# limpeza defensiva no arranque
pkill -9 -f "bin/controller" 2>/dev/null
pkill -9 -f "bin/runner" 2>/dev/null
rm -f tmp/*
sleep 0.5

echo "=========================================="
echo "  COMPARACAO FIFO vs ROUND-ROBIN"
echo "=========================================="

run_test() {
    local policy=$1
    local label=$2

    echo
    echo "─── Politica: $label ($policy) ───"

    # limpar tudo antes de cada teste
    pkill -9 -f "bin/controller" 2>/dev/null
    pkill -9 -f "bin/runner" 2>/dev/null
    rm -f tmp/*
    sleep 0.3

    # arrancar controller e GUARDAR o PID
    ./bin/controller 1 "$policy" > /dev/null 2>&1 &
    local CPID=$!
    sleep 0.5

    # alice submete 5 comandos
    for i in 1 2 3 4 5; do
        ./bin/runner -e alice "sleep 1" > /dev/null 2>&1 &
    done

    sleep 0.1
    ./bin/runner -e bob "sleep 1" > /dev/null 2>&1 &

    sleep 0.1
    ./bin/runner -e carol "sleep 1" > /dev/null 2>&1 &

    echo "  (a aguardar execucao...)"

    # esperar só pelos runners, não pelo controller
    for pid in $(jobs -p); do
        if [ "$pid" != "$CPID" ]; then
            wait "$pid" 2>/dev/null
        fi
    done

    echo "  (runners terminaram)"

    # dar tempo ao controller para escrever o ultimo log
    sleep 0.3

    # matar controller pelo PID
    kill -9 "$CPID" 2>/dev/null
    wait "$CPID" 2>/dev/null

    echo "  Ordem de execucao:"
    awk -F'[ =]' '{print "    "$2}' tmp/history.log

    echo
    echo "  Tempo medio por utilizador:"
    awk -F'[ =]' '{
        sum[$2] += $6
        count[$2]++
    }
    END {
        for (u in sum) printf "    %-10s %.2fs  (%d comandos)\n", u, sum[u]/count[u], count[u]
    }' tmp/history.log
}

run_test fifo "FIFO"
run_test rr   "Round-Robin"

# limpeza final
pkill -9 -f "bin/controller" 2>/dev/null
pkill -9 -f "bin/runner" 2>/dev/null
rm -f tmp/*

echo
echo "=========================================="
echo "  ANALISE:"
echo "  - FIFO: bob e carol esperam muito tempo"
echo "  - RR: distribui mais justamente entre users"
echo "=========================================="