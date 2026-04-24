#!/bin/bash
# test_basic.sh — testa funcionalidades básicas do sistema
cd "$(dirname "$0")/.."

PASS=0
FAIL=0

ok()   { echo "[PASS] $1"; PASS=$((PASS+1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL+1)); }

start_ctl() {
    rm -f tmp/*
    ./bin/controller "$1" "$2" &
    CTL_PID=$!
    sleep 0.3
}

stop_ctl() {
    kill $CTL_PID 2>/dev/null
    wait $CTL_PID 2>/dev/null
}

echo "=========================================="
echo "  TESTES BASICOS — controller / runner"
echo "=========================================="

# T1: echo simples 
echo; echo "[T1] echo simples"
start_ctl 1 fifo
OUT=$(./bin/runner -e user1 "echo ola" 2>&1)
stop_ctl
echo "$OUT" | grep -q "ola" \
    && ok "T1: output 'ola' presente" \
    || fail "T1: output ausente"

# T2: query vazia 
echo; echo "[T2] query sem comandos"
start_ctl 1 fifo
OUT=$(./bin/runner -c 2>&1)
stop_ctl
echo "$OUT" | grep -q "Executing" && ok "T2: campo Executing presente" || fail "T2: Executing ausente"
echo "$OUT" | grep -q "Scheduled" && ok "T2: campo Scheduled presente" || fail "T2: Scheduled ausente"

# T3: query durante execucao 
echo; echo "[T3] query com comando em execucao"
start_ctl 1 fifo
./bin/runner -e alice "sleep 1" >/dev/null 2>&1 &
RPID=$!
sleep 0.3
OUT=$(./bin/runner -c 2>&1)
wait $RPID 2>/dev/null
stop_ctl
echo "$OUT" | grep -q "alice" \
    && ok "T3: utilizador visivel em Executing" \
    || fail "T3: utilizador nao aparece na query"

# T4: shutdown idle 
echo; echo "[T4] shutdown sem tarefas"
start_ctl 1 fifo
./bin/runner -s >/dev/null 2>&1
sleep 0.3
kill -0 $CTL_PID 2>/dev/null \
    && fail "T4: controller ainda a correr" \
    || ok  "T4: controller terminou"

# T5: log gerado 
echo; echo "[T5] log history.log criado"
start_ctl 1 fifo
./bin/runner -e loguser "echo teste" >/dev/null 2>&1
stop_ctl
[ -f tmp/history.log ] && grep -q "loguser" tmp/history.log \
    && ok "T5: log contem entrada do utilizador" \
    || fail "T5: log ausente ou incorreto"

echo
echo "=========================================="
echo "  RESULTADO: $PASS passaram, $FAIL falharam"
echo "=========================================="
[ $FAIL -eq 0 ] && exit 0 || exit 1