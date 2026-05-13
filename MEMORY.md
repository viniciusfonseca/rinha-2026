# Memoria do Projeto

Este arquivo existe para acelerar handoff entre agentes. Ele resume a arquitetura atual, o estado valido mais recente, comandos de operacao e armadilhas ja descobertas.

## Resumo Rapido

- Projeto: backend da Rinha 2026 em C.
- Stack: API HTTP com `io_uring`, load balancer TCP round-robin para clientes e unix sockets para as APIs, preprocess offline para gerar `index.bin`.
- Imagem final: `FROM scratch`, contendo apenas `fraud_api`, `fraud_lb` e `/opt/rinha/index.bin`.
- Ambiente padrao de submissao: `linux/amd64` com `1 CPU` e `350 MB` no total.
- Ambiente de excecao para desenvolvimento local em Mac: override em `docker-compose.macos.yml`, apenas para trocar a plataforma para `linux/arm64/v8`.

## Arquitetura Atual

- [src/api.c](/Users/viniciusfonseca/projects/rinha-2026/src/api.c)
  - Servidor HTTP em `io_uring`.
  - Exponibiliza `GET /ready` e `POST /fraud-score`.
  - Mantem `keep-alive` por padrao, a menos que receba `Connection: close`.
  - Usa `generation` em `user_data` para descartar CQEs antigos quando um slot de conexao e reutilizado.
  - Agora drena CQEs em lote e so faz flush de SQEs ao final de cada lote.
  - Faz parse HTTP, vetorizacao, consulta ao indice e resposta JSON.

- [src/lb.c](/Users/viniciusfonseca/projects/rinha-2026/src/lb.c)
  - Proxy TCP round-robin para clientes e proxy via unix sockets para as APIs.
  - Transparente para HTTP; nao interpreta payload.
  - Usa identificador com `generation` em `user_data` para descartar CQEs antigos e evitar corrupcao em reuse de sessoes.
  - Agora drena CQEs em lote e so faz flush de SQEs ao final de cada lote.
  - Foi ajustado para caber no limite de memoria do LB.

- [src/vectorize.c](/Users/viniciusfonseca/projects/rinha-2026/src/vectorize.c)
  - Converte o payload da Rinha em vetor de 14 dimensoes.

- [src/preprocess.c](/Users/viniciusfonseca/projects/rinha-2026/src/preprocess.c)
  - Baixa o dataset oficial no build da imagem e gera `index.bin`.
  - Hoje gera estrutura IVF enxuta com centroides, offsets por lista, raios por lista, labels e vetores quantizados em `16 bits`.

- [src/index.c](/Users/viniciusfonseca/projects/rinha-2026/src/index.c)
  - Consulta o `index.bin`.
  - Estrategia atual: aquecer a busca com as `nprobe` listas de centroides mais proximos e depois expandir as listas restantes em ordem de `lower bound`, parando cedo quando o `top-5` ja esta matematicamente fechado.
  - O loop quente de distancia em x86 faz dequantizacao AVX2 direta em registrador, sem `gather` na LUT, para reduzir custo de CPU/cache no runtime.
  - O erro residual relevante vinha da representacao dos vetores, nao mais do algoritmo aproximado de busca.
  - Em x86, o hot path de distancia usa SIMD AVX2 com fallback scalar em outras arquiteturas.

- [src/common.h](/Users/viniciusfonseca/projects/rinha-2026/src/common.h)
  - Parametros globais do indice e quantizacao em 16 bits.
  - Estado atual importante:
    - `RINHA_IVF_NLIST = 512`
    - `RINHA_IVF_NPROBE = 4`
    - `RINHA_IVF_TRAIN_SAMPLES = 65536`
    - `RINHA_IVF_KMEANS_ITERS = 16`
    - vetores armazenados em `uint16_t` via `rinha_vector_scalar_t`

## Formato do Indice

- Arquivo: [src/index_format.h](/Users/viniciusfonseca/projects/rinha-2026/src/index_format.h)
- Versao atual:
  - `RINHA_INDEX_MAGIC = "R26IVF9"`
  - `RINHA_INDEX_VERSION = 9`
- Mudancas mais recentes:
  - inclusao de `list_radii`
  - armazenamento de vetores quantizados em 16 bits
  - remocao do payload morto de `PQ` do arquivo serializado

Sempre que mudar o formato serializado, atualizar esse header e regenerar `index.bin`.

## Estado Validado Mais Recente

Ultima rodada forte validada no ambiente equivalente ao oficial em Mac:

- compose: `docker-compose.yml` + `docker-compose.macos.yml`
- plataforma: `linux/arm64/v8`
- limites preservados do ambiente oficial: `1 CPU` e `350 MB`
- resultado em [test/results.json](/Users/viniciusfonseca/projects/rinha-2026/test/results.json):
  - `p99 = 4.21ms`
  - `http_errors = 0`
  - `false_positive_detections = 0`
  - `false_negative_detections = 1`
  - `failure_rate = 0%` no relatorio arredondado
  - `final_score = 5195.37`

Imagem local validada apos essa rodada:
- `rinha-2026-local`
- `Size = 35,399,427` bytes em `arm64`

Importante:
- O `0%` de `failure_rate` vem de arredondamento.
- Ainda existe `1` falso negativo no breakdown.
- O caminho SIMD em x86 nao altera o comportamento funcional; no Mac arm64 ele cai no fallback scalar.

## Comandos Uteis

- Build local dos binarios:
  - `make all`

- Subir stack padrao de submissao:
  - `make up`

- Derrubar stack padrao:
  - `make down`

- Rodar CI local padrao:
  - `make test-ci`

- Subir stack em Mac com os mesmos limites da Rinha:
  - `make up-macos`

- Rodar carga em Mac com os mesmos limites da Rinha:
  - `make test-ci-macos`

- Rodar smoke manual:
  - `curl -sS -D - -o /dev/null http://localhost:9999/ready`
  - `curl -sS -D - -H "Content-Type: application/json" --data-raw @payload.json http://localhost:9999/fraud-score`

## Compose e Ambientes

- [docker-compose.yml](/Users/viniciusfonseca/projects/rinha-2026/docker-compose.yml)
  - Base oficial.
  - `platform: linux/amd64`.
  - Orcamento total da Rinha dividido entre `lb`, `api1` e `api2`.
  - `lb` e `api1/api2` compartilham um volume com sockets unix em `/run/rinha`.

- [docker-compose.macos.yml](/Users/viniciusfonseca/projects/rinha-2026/docker-compose.macos.yml)
  - Excecao para desenvolvimento local em Mac.
  - Usa `linux/arm64/v8`.
  - Mantem o mesmo desenho de recursos do compose oficial.

## Armadilhas Ja Descobertas

- `io_uring` em Docker Desktop no Mac com `linux/amd64` pode falhar em runtime via emulacao.
  - Para Mac, use os overrides dedicados.

- O `LB` ja estourou memoria quando as sessoes e buffers estavam superdimensionados.
  - O estado atual foi ajustado para caber no limite.
  - Nao aumente `LB_MAX_SESSIONS` ou `LB_BUFFER_SIZE` sem revalidar memoria.

- O `LB` ja teve bug de reuse de sessao com CQEs antigos.
  - Preserve a logica de `generation` em `user_data`.

- A comunicacao interna `LB -> API` agora usa unix sockets.
  - Os caminhos padrao sao `/run/rinha/api1.sock` e `/run/rinha/api2.sock`.
  - O compose monta um volume compartilhado em `/run/rinha`.

- A API ja caiu em timeouts quando a busca degenerava para custo alto por request.
  - Hoje a busca esta mais previsivel com listas ordenadas por lower bound, parada antecipada e poda por raio.

- O `README.md` esta desatualizado em partes.
  - Ele ainda menciona LSH como estrategia principal.
  - Para o estado atual, confie mais em `src/index.c`, `src/preprocess.c`, `src/common.h` e neste arquivo.

- O build da imagem depende de rede.
  - [Dockerfile](/Users/viniciusfonseca/projects/rinha-2026/Dockerfile) baixa `references.json.gz` do repositorio oficial durante o build.

## Invariantes Importantes

- A imagem final deve continuar `FROM scratch`.
- A imagem final deve conter somente:
  - `/usr/local/bin/fraud_api`
  - `/usr/local/bin/fraud_lb`
  - `/opt/rinha/index.bin`
- `GET /ready` deve responder `204`.
- `POST /fraud-score` deve responder JSON com `approved` e `fraud_score`.
- `docker-compose.yml` deve permanecer como baseline `linux/amd64`.
- O caminho de Mac deve continuar sendo override, nao o padrao.

## Arquivos Mais Importantes

- [Makefile](/Users/viniciusfonseca/projects/rinha-2026/Makefile)
- [Dockerfile](/Users/viniciusfonseca/projects/rinha-2026/Dockerfile)
- [docker-compose.yml](/Users/viniciusfonseca/projects/rinha-2026/docker-compose.yml)
- [docker-compose.macos.yml](/Users/viniciusfonseca/projects/rinha-2026/docker-compose.macos.yml)
- [src/api.c](/Users/viniciusfonseca/projects/rinha-2026/src/api.c)
- [src/lb.c](/Users/viniciusfonseca/projects/rinha-2026/src/lb.c)
- [src/preprocess.c](/Users/viniciusfonseca/projects/rinha-2026/src/preprocess.c)
- [src/index.c](/Users/viniciusfonseca/projects/rinha-2026/src/index.c)
- [src/common.h](/Users/viniciusfonseca/projects/rinha-2026/src/common.h)
- [test/test.js](/Users/viniciusfonseca/projects/rinha-2026/test/test.js)
- [test/results.json](/Users/viniciusfonseca/projects/rinha-2026/test/results.json)

## Quando For Mexer No Indice

1. Atualize os parametros em `src/common.h` se necessario.
2. Se houver mudanca de serializacao, ajuste `src/index_format.h`.
3. Garanta compatibilidade entre `src/preprocess.c` e `src/index.c`.
4. Rebuild completo da imagem para regenerar `index.bin`.
5. Revalide com smoke e com `make test-ci` ou `make test-ci-macos`.

## Proximos Pontos Naturais de Trabalho

- Tentar eliminar o ultimo falso negativo sem reintroduzir timeout.
- Atualizar o `README.md` para refletir a estrategia atual do indice.
- Se a qualidade travar novamente, investigar custo/beneficio de armazenar mais informacao por vetor ou refinamento final ainda mais seletivo.
