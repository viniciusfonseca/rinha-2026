# AGENTS

Guia rapido para agentes que forem trabalhar neste repositorio.

Leia este arquivo primeiro. Para estado operacional mais recente, resultados de teste e detalhes de tuning, leia tambem `MEMORY.md`.

## Objetivo do Projeto

Este repositorio implementa a solucao da Rinha de Backend 2026 em C, com:

- API HTTP baseada em `io_uring`
- load balancer TCP round-robin para clientes e unix sockets para as APIs, baseado em `io_uring`
- preprocess offline que gera `index.bin`
- imagem final `FROM scratch`

## Arquitetura

- `src/api.c`
  - atende `GET /ready` e `POST /fraud-score`
  - usa `generation` em `user_data` para evitar reaproveitamento incorreto de CQEs
  - faz parse HTTP, vetorizacao, consulta ao indice e resposta JSON

- `src/lb.c`
  - proxy TCP entre cliente e LB, e unix sockets entre LB e APIs
  - mantem conexoes ativas e distribui em round-robin

- `src/vectorize.c`
  - converte o payload da Rinha em um vetor de 14 dimensoes

- `src/preprocess.c`
  - gera `index.bin` a partir do dataset oficial

- `src/index.c`
  - abre e consulta o indice vetorial serializado

- `src/common.h`
  - concentra parametros globais e tipos de quantizacao

## Estado Atual da Busca

O projeto nao esta mais na estrategia antiga baseada em LSH como caminho principal de consulta.

O estado atual e:

- indice IVF serializado em `index.bin`
- consulta com aquecimento das `nprobe` listas de centroides mais proximos
- expansao exata so para listas que ainda podem melhorar o `top-5`
- varredura exata dentro das listas com poda por raio
- arquivo serializado sem payload morto de `PQ`
- vetores armazenados em 16 bits para reduzir erro de representacao

Se encontrar documentacao antiga mencionando LSH como estrategia principal, trate como desatualizada e confirme no codigo atual.

## Ambiente e Compose

- `docker-compose.yml`
  - baseline oficial
  - `linux/amd64`
  - deve continuar sendo o caminho padrao
  - monta um volume compartilhado em `/run/rinha` para os sockets internos

- `docker-compose.macos.yml`
  - excecao para desenvolvimento local em Mac
  - usa `linux/arm64/v8`
  - mantem os mesmos limites do compose oficial, mudando apenas a plataforma

Nao transforme o caminho de Mac no padrao. O ambiente-alvo da competicao e `linux/amd64`.

## Invariantes Importantes

- a imagem final deve continuar `FROM scratch`
- a imagem final deve conter somente:
  - `/usr/local/bin/fraud_api`
  - `/usr/local/bin/fraud_lb`
  - `/opt/rinha/index.bin`
- `GET /ready` deve responder `204`
- `POST /fraud-score` deve responder com `approved` e `fraud_score`
- `docker-compose.yml` deve permanecer como baseline `linux/amd64`
- mudancas no formato do indice exigem atualizar `src/index_format.h` e regenerar `index.bin`

## Comandos Principais

- build local:
  - `make all`

- subir stack padrao:
  - `make up`

- derrubar stack padrao:
  - `make down`

- teste CI local padrao:
  - `make test-ci`

- stack local em Mac:
  - `make up-macos`

- teste de carga em Mac:
  - `make test-ci-macos`

## Armadilhas Conhecidas

- `io_uring` com Docker Desktop no Mac em `linux/amd64` pode falhar via emulacao
- a comunicacao interna LB -> API usa unix sockets em `/run/rinha`
- o LB ja estourou memoria quando buffers e sessoes estavam grandes demais
- o LB ja teve bug de reuse de sessao com CQEs antigos; preserve a logica de `generation`
- o build da imagem depende de rede para baixar `references.json.gz`
- `README.md` pode ficar atrasado em relacao ao estado real do indice

## Quando Mexer No Indice

1. Atualize parametros em `src/common.h` se necessario.
2. Se o formato serializado mudar, atualize `src/index_format.h`.
3. Mantenha `src/preprocess.c` e `src/index.c` compativeis entre si.
4. Rebuild a imagem para regenerar `index.bin`.
5. Revalide com smoke test e com `make test-ci` ou `make test-ci-macos`.

## Handoff Esperado

Ao concluir mudancas relevantes:

- atualize `MEMORY.md` se a arquitetura, tuning, formato do indice ou fluxo operacional tiver mudado
- registre o resultado mais recente em `test/results.json` quando fizer validacao de carga
- sinalize qualquer risco residual, especialmente latencia, memoria e compatibilidade de plataforma

## Leitura Recomendada

Antes de fazer mudancas grandes, leia nesta ordem:

1. `AGENTS.md`
2. `MEMORY.md`
3. `src/common.h`
4. `src/index_format.h`
5. `src/index.c`
6. `src/preprocess.c`
7. `Makefile`
8. `docker-compose.yml`
