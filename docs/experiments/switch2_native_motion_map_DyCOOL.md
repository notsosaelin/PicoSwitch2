# Switch 2 Joy-Con 2 (R) native motion — decoded map v2

**Data:** 2026-07-10
**Fontes:** `switch2_two_controller_20260710_064201.pkl` (dual, referência common do JC-L no grip) + `switch2_guided_single_20260710_132345.pkl` (captura guiada de eixo isolado, 11 464 reports).
**Decoder de referência:** `analysis/s2decode.py`

## Estrutura (report nativo 0x08, BLE handle 0x000E)

```
motion  = report[0x10:0x38]      # 40 bytes
payload = motion[4:40]           # 36 bytes = 288 bits, LSB-first
```

`motion[0:2]`: bits 0–11 tick interno (~800 Hz), bits 12–15 elapsed ticks.
`motion[2:4]`: temperatura (int16 LE).

### Payload — layout normal (elapsed ∈ {11,12,13}, ≈96 % dos reports)

| bits | campo | formato | escala | instante |
|---|---|---|---|---|
| 0 | ? (sempre 1) | 1 bit | — | — |
| 1 | flag taxa alta | 1 bit | 1=normal, 0=perto/em clamp | — |
| 2–4 | zeros | | | |
| 5–67 | header: contador + região de deriva lenta (não decodificado; não necessário para IMU) | | | |
| 68–110 | **accel A** | 3×int14 | 4096 counts/g | t−10 ms |
| 110–149 | **gyro M** | 3×int13 | 8.2 counts/(°/s) (= metade da resolução de G) | t−5 ms |
| 149–188 | **accel B** | 3×int13 | 2048 counts/g (meia resolução) | t−5 ms |
| 188–230 | **gyro G** | 3×int14 | **16.4 counts/(°/s)** | t (mais recente) |
| 230–272 | **accel C** | 3×int14 | 4096 counts/g | t |
| 272–288 | tail/status (muda com o formato; descritor) | 16 bits | | |

Ordem dos eixos: X, Y, Z em todos os blocos (X=pitch/nariz-cima-baixo confirmado; Z=yaw confirmado; Y por confirmar com rotação limpa do eixo longo).

### Escalas e limites

- Gyro nativo: **16.4 counts/(°/s)** — escala raw do sensor ±2000 dps.
  Confirmada por integração vs gravidade: pitch lento deu 16.16 e 16.54.
- **Clamp a ±500 °/s**: os campos transmitidos são 14 bits (±8192 counts) e
  saturam com correção de bias (valores presos em ≈±8184, não ±8191).
  O bit 1 do payload cai para 0 quando a taxa se aproxima do clamp.
- Accel nativo: 4096 counts/g (|g| em repouso medido: 4131).
- Gyro do stream COMMON (0x05): **≈14.3 counts/(°/s)** (0.070 °/s/count,
  estilo Switch 1). A nota antiga "48000 counts = 360°/s" está **errada**
  (medido por integração vs gravidade: mediana 14.09).
- Accel common: 4096 counts/g (correto como documentado).
- Relação native/common medida: slope 0.871–0.888 ≈ 14.3/16.4 ✓.

### Amostragem

Report BLE ~64 Hz cobre ~12 ticks de 1.25 ms (~800 Hz interno).
Accel: 3 amostras/report (~200 Hz), A e C a resolução completa, B a metade.
Gyro: 2 amostras/report (~133 Hz), M (meio, 13 bits) e G (fim, 14 bits).

### Layout catch-up (~3–5 % dos reports)

Quando um report cobre ~2 intervalos de ligação (Δtick 24–30; o nibble
elapsed é apenas `Δtick mod 16`, por isso aparece 8–14 — detetar SEMPRE
pelo Δtick face ao report anterior), o payload usa campos mais largos:

| bits | campo | formato | escala |
|---|---|---|---|
| 68–110 | accel 1 | 3×int14 | 4096/g |
| 110–155 | gyro 1 (amostra antiga) | 3×int15 | parcialmente validado |
| 155–197 | accel 2 | 3×int14 | 4096/g ✅ |
| 197–245 | **gyro 2 (mais recente)** | 3×int16 | valor/4 = unidades G ✅ (corr 0.88–0.97) |
| 245–287 | accel 3 | 3×int14 | 4096/g ✅ |

Soma: 68+42+45+42+48+42+1 = 288 ✓. Com 16 bits ÷4 → mesmo clamp ±500 °/s.
Prático: usar gyro 2 e accel 3; ignorar gyro 1.

### Joy-Con L (report 0x07)

O bloco motion começa em **report[0x0F]** (não 0x10). O payload tem o
MESMO layout do R (validado: parado σ=1–5 counts, gravidade 4054, eixos
X=pitch e Z=yaw confirmados fisicamente).

## Validação

- gyro G e M vs gyro common do JC-L (grip, timestamps deslocados +12.5/+7.5 ms):
  corr 0.996–0.9994 nos 3 eixos (teto do ruído entre sensores distintos).
- accel A/B/C vs accel common: 0.999.
- M vs G (mesmas unidades): slope 0.989–0.996.
- Em repouso: gyro médio (−1.2, −5.3, −2.3) counts; σ = (4.9, 9.2, 3.8).

## Confiança

| item | estado |
|---|---|
| accel A/C 14b @68/@230, 4096/g | CONFIRMADO |
| accel B 13b @149, meia resolução | CONFIRMADO |
| gyro G 14b @188/202/216, 16.4 counts/dps | CONFIRMADO |
| gyro M 13b @110/123/136, meia resolução | CONFIRMADO |
| clamp ±500 dps + flag bit 1 | FORTE |
| eixo Y | CONFIRMADO via correspondência eixo-a-eixo na captura dual (0.996 vs gyro Y do JC de referência); rotação física isolada em Y nunca capturada — validação direta pendente |
| layout catch-up (Δtick>16): gyro2 16b@197 ÷4, accels 14b @68/155/245 | CONFIRMADO (gyro1 15b@110 PARCIAL) |
| deteção de layout por Δtick (nibble elapsed = Δtick mod 16) | CONFIRMADO |
| Joy-Con L 0x07: motion @0x0F, mesmo layout | CONFIRMADO |
| header 5–67, tail 272–287 | DESCONHECIDO (não necessário p/ IMU) |
| Pro Controller 2 (0x09) | POR VERIFICAR (motion @0x0F, 30 bytes USB — payload menor, provável variante) |

## Próximas experiências (opcionais)

1. Validação direta do eixo Y: comando apontado para a frente como uma chave
   de fenda (eixo longo horizontal), torcer lentamente — NÃO com o comando
   plano em cima da mesa (isso é yaw/Z).
2. Pro Controller 2 via USB/BLE → verificar variante de 30 bytes.
3. Mapear gyro1 do layout catch-up (precisa de rotação constante + comandos
   enviados a meio para gerar catch-ups em movimento previsível).
4. Header 5–67 (região de deriva lenta — talvez fusão/orientação interna).
