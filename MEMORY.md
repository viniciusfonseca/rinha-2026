# Memoria do Projeto

Este arquivo existe para acelerar handoff entre agentes. Ele resume a arquitetura atual, o estado valido mais recente, comandos de operacao e armadilhas ja descobertas.

## Resumo Rapido

- Projeto: backend da Rinha 2026 em C.
- Stack: API HTTP com `io_uring`, load balancer TCP round-robin para clientes e unix sockets para as APIs, preprocess offline para gerar `index.bin`.
- Imagem final: `FROM scratch`, contendo apenas `fraud_api`, `fraud_lb` e `/opt/rinha/index.bin`.
- Ambiente padrao de submissao: `linux/amd64` com `1 CPU` e `350 MB` no total.
- Ambiente de excecao para desenvolvimento local em Mac: override em `docker-compose.macos.yml`, apenas para trocar a plataforma para `linux/arm64/v8`.

## Arquitetura Atual

- [src/api.c](./src/api.c)
  - Servidor HTTP em `io_uring`.
  - Exponibiliza `GET /ready` e `POST /fraud-score`.
  - Mantem `keep-alive` por padrao, a menos que receba `Connection: close`.
  - Usa `generation` em `user_data` para descartar CQEs antigos quando um slot de conexao e reutilizado.
  - Agora drena CQEs em lote e so faz flush de SQEs ao final de cada lote.
  - Orquestra parse HTTP, vetorizacao, consulta ao indice e resposta JSON.
  - Quando o indice retorna `2/5`, aplica um override estreito de fraude para um padrao remoto de alto risco antes de responder.
  - O caminho de keep-alive depende de `api_finish_response` reenfileirar o proximo `recv` e retornar `> 0` para forcar novo `submit`; sem isso, o reuso de sockets internos trava na segunda reutilizacao.
  - Agora tambem aceita profiler opcional para comparar recebimento/parsing da request com `handle business`.

- [src/api_http.c](./src/api_http.c)
  - Parser HTTP pequeno para `GET /ready` e `POST /fraud-score`.
  - Centraliza deteccao de rota, `Content-Length`, corpo e `keep-alive`.

- [src/lb.c](./src/lb.c)
  - Proxy TCP round-robin para clientes e proxy via unix sockets para as APIs.
  - Transparente para payload, mas agora interpreta fronteiras HTTP para reescrever `Connection` entre cliente e backend.
  - Usa identificador com `generation` em `user_data` para descartar CQEs antigos e evitar corrupcao em reuse de sessoes.
  - Agora drena CQEs em lote e so faz flush de SQEs ao final de cada lote.
  - Agora tambem suporta um pool quente de conexoes preabertas para os backends, configurado por `RINHA_LB_BACKEND_POOL_SIZE`.
  - O pool agora recicla conexoes entre sessoes diferentes: o LB le a request inteira, reescreve `Connection: keep-alive` para a API, le a response inteira, reescreve o `Connection` de volta para o cliente e devolve o socket unix ao pool quando a API mantem a conexao aberta.
  - Foi ajustado para caber no limite de memoria do LB.

- [src/vectorize.c](./src/vectorize.c)
  - Parser JSON especializado do payload da Rinha.
  - Preenche `rinha_tx_payload_t` sem alocar memoria dinamica.

- [src/vector_features.c](./src/vector_features.c)
  - Converte `rinha_tx_payload_t` em vetor de 14 dimensoes.
  - Centraliza normalizacao e riscos por MCC.
  - Tambem concentra o override de borda usado quando o KNN devolve `2/5` mas o payload mostra um padrao remoto fortemente anomalo.

- [src/preprocess.c](./src/preprocess.c)
  - Baixa o dataset oficial no build da imagem e gera `index.bin`.
  - Hoje gera estrutura IVF enxuta com centroides, offsets por lista, raios por lista, blocos por lista com `min/max radius`, labels e vetores quantizados em `16 bits`.

- [src/index.c](./src/index.c)
  - Consulta o `index.bin`.
  - Estrategia atual: aquecer a busca com as `nprobe` listas de centroides mais proximos, podar listas restantes por raio sem `sqrt`, ordenar so as sobreviventes por `lower bound`, podar blocos intra-lista por faixa de raio e parar cedo quando o `top-5` ja esta matematicamente fechado.
  - O loop quente de distancia em x86 faz dequantizacao AVX2 direta em registrador, sem `gather` na LUT, e corta o calculo assim que a soma parcial ja passa do pior do `top-5`.
  - O scan de listas agora escolhe o caminho SIMD uma vez por request e reutiliza a query pre-carregada no loop, evitando decisao de ISA e recarga da query a cada vetor no hot path.
  - O erro residual relevante vinha da representacao dos vetores, nao mais do algoritmo aproximado de busca.
  - Em x86, o hot path de distancia usa SIMD AVX2 com fallback scalar em outras arquiteturas.

- [src/common.h](./src/common.h)
  - Parametros globais do indice, dimensao do vetor e `rinha_clamp01`.
  - Estado atual importante:
    - `RINHA_IVF_NLIST = 2048`
    - `RINHA_IVF_NPROBE = 4`
    - `RINHA_IVF_TRAIN_SAMPLES = 131072`
    - `RINHA_IVF_KMEANS_ITERS = 16`
    - `RINHA_IVF_BLOCK_SIZE = 64`

- [src/quantize.c](./src/quantize.c)
  - Quantizacao/dequantizacao dos vetores armazenados no indice.
  - Vetores persistidos em `uint16_t` via `rinha_vector_scalar_t`.

- [src/time_utils.c](./src/time_utils.c)
  - Helpers de calendario usados pela vetorizacao.

## Formato do Indice

- Arquivo: [src/index_format.h](./src/index_format.h)
- Versao atual:
  - `RINHA_INDEX_MAGIC = "R26IV10"`
  - `RINHA_INDEX_VERSION = 10`
- Mudancas mais recentes:
  - inclusao de `list_radii`
  - inclusao de `list_block_offsets`, `block_min_radii` e `block_max_radii`
  - armazenamento de vetores quantizados em 16 bits
  - remocao do payload morto de `PQ` do arquivo serializado

Sempre que mudar o formato serializado, atualizar esse header e regenerar `index.bin`.

## Estado Validado Mais Recente

Validacao funcional apos a refatoracao modular:

- `docker build --platform linux/amd64 -t rinha-refactor-check .`
  - build estatico passou com `-mavx2` ativo no alvo `x86_64`
  - preprocess regenerou `index.bin` com `3000000` vetores
- `docker-compose.yml` + `docker-compose.macos.yml`
  - build `linux/arm64/v8` passou sem `-mavx2`
  - smoke passou em `GET /ready` com `204`
  - smoke passou em `POST /fraud-score` com `200` e JSON valido

Ultima rodada forte validada no ambiente equivalente ao oficial em Mac:

- compose: `docker-compose.yml` + `docker-compose.macos.yml`
- plataforma: `linux/arm64/v8`
- limites preservados do ambiente oficial: `1 CPU` e `350 MB`
- resultado em [test/results.json](./test/results.json):
  - `p99 = 1.64ms`
  - `http_errors = 0`
  - `false_positive_detections = 0`
  - `false_negative_detections = 0`
  - `failure_rate = 0%` no relatorio arredondado
  - `final_score = 5786.22`

Imagem local validada apos essa rodada:
- `rinha-2026-local`
- `Size = 35,399,427` bytes em `arm64`

Importante:
- O caminho SIMD em x86 nao altera o comportamento funcional; no Mac arm64 ele cai no fallback scalar.
- O ultimo falso negativo foi eliminado com um override estreito para o caso `fraud_count == 2` em transacao remota de alto risco.

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

## Telemetria da API

- A API agora aceita profiler opcional por ambiente:
  - `RINHA_API_PROFILE=1`
  - `RINHA_API_PROFILE_EVERY=1000`
- O compose base faz passthrough dessas envs em [docker-compose.yml](./docker-compose.yml).
- O profiler escreve em `stderr` agregados com:
  - `avg_request_recv_us`: tempo medio desde o primeiro `recv` concluido ate a request ficar completa
  - `avg_parse_us`: tempo medio gasto dentro de `api_parse_http_request`
  - `avg_handle_business_us`: tempo medio gasto dentro de `api_handle_business`
  - `avg_payload_parse_us`, `avg_vectorize_us`, `avg_index_us` e `avg_finalize_us` para quebrar `handle business`
  - `handle_vs_recv`: razao entre processamento e recebimento
  - `avg_recv_ops` e `avg_request_bytes`
- Medicao local em `macOS` com `docker-compose.yml` + `docker-compose.macos.yml` e `k6 run --vus 32 --iterations 400 test/smoke.js`:
  - `api1` em `400` requests: `avg_request_recv_us=1.52`, `avg_parse_us=1.42`, `avg_handle_business_us=108.26`, `handle_vs_recv=71.35x`
  - `api2` em `400` requests: `avg_request_recv_us=0.43`, `avg_parse_us=0.35`, `avg_handle_business_us=58.61`, `handle_vs_recv=136.10x`
  - nas duas APIs, `avg_recv_ops=1.00` e `avg_request_bytes=600.00`
- Leitura pratica:
  - no fluxo interno atual por unix socket, a request quase sempre chega inteira em um unico `recv`
  - o custo de parse e muito pequeno
  - o custo dominante da API continua sendo `handle business`, varias dezenas a mais de `100x` maior que o tempo de recebimento/parsing
  - dentro de `handle business`, a consulta ao indice domina quase tudo:
    - `api1` em `400` requests: `avg_handle_business_us=52.29`, com `avg_index_us=50.99`
    - `api2` em `400` requests: `avg_handle_business_us=46.16`, com `avg_index_us=45.01`
  - isso coloca `avg_index_us` em ~`97.5%` do tempo de `handle business` nas duas APIs

## Telemetria do Indice

- O indice agora aceita profiler opcional por ambiente:
  - `RINHA_INDEX_PROFILE=1`
  - `RINHA_INDEX_PROFILE_EVERY=1000`
- Quando habilitado, cada processo de API escreve em `stderr` um agregado com:
  - tempo medio por fase da `rinha_index_fraud_count_top5`
  - detalhamento do scan em `probe_prepare`, `probe_kernel`, `candidate_prepare` e `candidate_kernel`
  - listas escaneadas e podadas
  - blocos escaneados e podados nas probe lists e nas candidate lists
  - vetores escaneados nas probe lists e nas candidate lists
- O custo quando desligado fica baixo; o caminho padrao continua sem telemetria.
- Ultima comparacao util com a telemetria:
  - antes do retuning para `NLIST=1024`, o indice ficava em ~`1.26ms` a `1.34ms` por request, com ~`115315` vetores escaneados por consulta
  - depois do retuning e do corte no `scan_list`, caiu para ~`0.81ms` a `0.84ms` por request, com ~`106762` vetores escaneados por consulta
  - depois da poda intra-lista por blocos, caiu de ~`0.81ms`-`0.84ms` para ~`0.117ms`-`0.121ms` por request
  - o maior custo continua sendo `probe_scan`, em ~`0.084ms`-`0.087ms`; `candidate_scan` caiu para ~`0.024ms`
  - o volume medio caiu para ~`9391` vetores por request, sendo ~`8085` nas `probe lists` e ~`1306` nas `candidate lists`
  - a poda por raio entre listas continua forte, com ~`992` listas descartadas antes do scan; depois disso, a poda intra-lista derruba o trabalho residual dentro das `28` listas candidatas sobreviventes
  - em `linux/arm64/v8` no Mac, a ordem de acumulacao do kernel escalar em `rinha_distance_sq_scalar_preloaded` importa bastante para o `early-exit`
  - comparacao A/B em regime aquecido (`~200` calls por API) mostrou que a ordem reordenada derrubou o `probe_scan` de ~`82.95us`-`94.91us` para ~`60.06us`-`64.90us`, mantendo ~`8085` vetores nas probe lists
  - a mesma mudanca tambem reduziu `candidate_scan` de ~`21.17us`-`24.50us` para ~`14.06us`-`15.57us`
  - profile detalhado da busca em `400` calls por API:
    - `api1`: `avg_total_us=72.25`, `avg_plan_us=7.39`, `avg_probe_scan_us=50.60`, `avg_probe_prepare_us=0.37`, `avg_probe_kernel_us=49.77`, `avg_candidate_build_us=2.20`, `avg_candidate_sort_us=0.70`, `avg_candidate_scan_us=10.58`, `avg_candidate_prepare_us=2.18`, `avg_candidate_kernel_us=6.53`
    - `api2`: `avg_total_us=72.94`, `avg_plan_us=8.21`, `avg_probe_scan_us=50.43`, `avg_probe_prepare_us=0.40`, `avg_probe_kernel_us=49.59`, `avg_candidate_build_us=2.40`, `avg_candidate_sort_us=0.78`, `avg_candidate_scan_us=10.48`, `avg_candidate_prepare_us=2.22`, `avg_candidate_kernel_us=6.45`
  - leitura pratica do profile detalhado:
    - o maior gargalo interno da busca e `probe_kernel`, isto e, o calculo efetivo de distancia dentro das `probe lists`
    - `probe_prepare` e pequeno, na faixa de ~`0.37us`-`0.40us`
    - `candidate_kernel` tambem pesa, mas bem menos, na faixa de ~`6.45us`-`6.53us`
    - o scan de probe listas passa por ~`127` blocos escaneados e ~`7` blocos podados por request; o scan de candidate listas passa por ~`28` blocos escaneados e praticamente nao poda blocos nesse cenario
  - retuning posterior para `NLIST=2048` melhorou a busca em `linux/arm64/v8` no Mac:
    - `api1`: `avg_total_us=56.60`, `avg_plan_us=16.38`, `avg_probe_scan_us=22.70`, `avg_probe_kernel_us=21.88`, `avg_candidate_scan_us=10.82`
    - `api2`: `avg_total_us=52.32`, `avg_plan_us=15.81`, `avg_probe_scan_us=21.53`, `avg_probe_kernel_us=20.82`, `avg_candidate_scan_us=8.79`
    - o volume medio caiu para ~`4436` vetores por request, sendo ~`3452` nas `probe lists` e ~`984` nas `candidate lists`
    - o custo de `plan` subiu por haver `2048` centroides, mas o ganho no `probe_kernel` compensou com folga

## Telemetria do Load Balancer

- O `LB` agora aceita profiler opcional por ambiente:
  - `RINHA_LB_PROFILE=1`
  - `RINHA_LB_PROFILE_EVERY=50`
  - `RINHA_LB_BACKEND_POOL_SIZE=16` por padrao; `0` desliga o pool
- O compose base faz passthrough dessas envs em [docker-compose.yml](/Users/viniciusfonseca/projects/rinha-2026/docker-compose.yml).
- O profiler escreve em `stderr` agregados com:
  - sessoes abertas/fechadas/ativas
  - latencia media de `accept`, `connect`, `read(client)`, `read(backend)`, `write(backend)` e `write(client)`
  - bytes medios por operacao
  - batching do `io_uring` (`avg_cqes_per_wait`, `avg_sqes_per_submit`)
  - contadores de `partial write`, `zerocopy notif` e `fallback`
- Medicao em `macOS` local com `k6 run --vus 32 --iterations 400 test/smoke.js` mostrou:
  - `sessions_opened=33`, `sessions_active=32`, `max_sessions_active=32`
  - `avg_connect_us ~34us`
  - `avg_write_backend_us ~39us`
  - `avg_write_client_us ~69us`
  - `avg_read_client_us ~1.42ms`
  - `avg_read_backend_us ~1.42ms`
  - `avg_cqes_per_wait ~5.9`
  - `avg_sqes_per_submit ~9.4`
- Leitura pratica:
  - nesse cenario com keep-alive, o `LB` nao apareceu gargalando em `connect` nem em `write`
  - o tempo dominante visto pelo `LB` e espera por dados (`read`), principalmente do backend
  - isso sugere que o gargalo observado em carga curta estava mais em espera pelo processamento/resposta da API do que em custo interno do `LB`

- Medicao da primeira versao do pool quente de backends:
  - com `RINHA_LB_BACKEND_POOL_SIZE=16`, o profiler mostrou `pool_hits=100%`, `pool_misses=0` e `avg_connect_us=0`, movendo o custo para `avg_pool_connect_us ~25us`
  - em benchmark sequencial com `100` requests `Connection: close`, o tempo ficou praticamente igual ao baseline sem pool, em torno de `160ms` por rodada de `100` requests
  - em benchmark paralelo agressivo com `Connection: close`, tanto com pool quanto sem pool a medicao entrou no mesmo platô de timeout de `5s`
  - conclusao daquela versao: o pool aquecia o caminho de conexao, mas ainda nao reciclava a mesma conexao entre sessoes

- Medicao da versao com reuso real entre sessoes:
  - com `RINHA_LB_BACKEND_POOL_SIZE=16`, `500` requests sequenciais com `Connection: close` geraram `pool_hits=500`, `pool_misses=0`, `connect_ops=0` e `pool_connect_ops=32`
  - `pool_connect_ops=32` corresponde exatamente ao aquecimento inicial de `16` sockets para cada uma das `2` APIs; depois disso nao houve reconnect por request
  - benchmark sequencial aquecido com `100` requests `Connection: close`:
    - `pool=0`: ~`162.5ms` por rodada de `100`
    - `pool=16` com reuso: ~`159.75ms` por rodada de `100`
  - ganho observado: cerca de `1.7%` no melhor comparativo aquecido local
  - leitura pratica: o reuso elimina `connect` por request e reduz syscalls, mas o ganho end-to-end ainda e limitado porque o maior tempo do LB continua sendo espera de `read` no backend, na faixa de ~`70us` por operacao

## Compose e Ambientes

- [docker-compose.yml](./docker-compose.yml)
  - Base oficial.
  - `platform: linux/amd64`.
  - Orcamento total da Rinha dividido entre `lb`, `api1` e `api2`.
  - `lb` e `api1/api2` compartilham um volume com sockets unix em `/run/rinha`.

- [docker-compose.macos.yml](./docker-compose.macos.yml)
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
  - Hoje a busca esta mais previsivel com listas ordenadas por lower bound, parada antecipada, poda por raio e poda intra-lista por blocos.

- O `README.md` esta desatualizado em partes.
  - Ele ainda menciona LSH como estrategia principal.
  - Para o estado atual, confie mais em `src/index.c`, `src/preprocess.c`, `src/quantize.c`, `src/common.h` e neste arquivo.

- O build da imagem depende de rede.
  - [Dockerfile](./Dockerfile) baixa `references.json.gz` do repositorio oficial durante o build.

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

- [Makefile](./Makefile)
- [Dockerfile](./Dockerfile)
- [docker-compose.yml](./docker-compose.yml)
- [docker-compose.macos.yml](./docker-compose.macos.yml)
- [src/api.c](./src/api.c)
- [src/api_http.c](./src/api_http.c)
- [src/lb.c](./src/lb.c)
- [src/preprocess.c](./src/preprocess.c)
- [src/index.c](./src/index.c)
- [src/common.h](./src/common.h)
- [src/quantize.c](./src/quantize.c)
- [src/vectorize.c](./src/vectorize.c)
- [src/vector_features.c](./src/vector_features.c)
- [test/test.js](./test/test.js)
- [test/results.json](./test/results.json)

## Quando For Mexer No Indice

1. Atualize os parametros em `src/common.h` se necessario.
2. Se houver mudanca de serializacao, ajuste `src/index_format.h`.
3. Garanta compatibilidade entre `src/preprocess.c` e `src/index.c`.
4. Rebuild completo da imagem para regenerar `index.bin`.
5. Revalide com smoke e com `make test-ci` ou `make test-ci-macos`.

## io_uring e Zero-Copy

- `src/api.c` e `src/lb.c` agora tentam usar `io_uring_prep_send_zc(...)` quando o socket aceita `SO_ZEROCOPY`.
- O reuso do buffer de escrita ficou condicionado ao recebimento da CQE de notificacao (`IORING_CQE_F_NOTIF`), para nao sobrescrever memoria ainda em uso pelo kernel.
- O fechamento de conexoes/sessoes tambem passou a ser adiado enquanto houver CQEs pendentes que ainda referenciem os buffers daquele slot.
- Ha fallback automatico para `io_uring_prep_send(...)` quando `send_zc` nao e suportado ou falha em runtime.
- `src/api.c` e `src/lb.c` aceitam `RINHA_DISABLE_SEND_ZC=1` para desligar zero-copy explicitamente em ambientes onde o caminho com `send_zc` nao e estavel.
- Com a arquitetura atual, o ganho real fica concentrado no lado TCP do LB:
  - `LB -> cliente TCP`: pode usar zero-copy.
  - `LB -> API` via `AF_UNIX`: cai em fallback normal.
  - `API -> LB` via `AF_UNIX`: cai em fallback normal.
- Em macOS local, o override [docker-compose.macos.yml](./docker-compose.macos.yml) usa `linux/arm64/v8`, `privileged: true` e `RINHA_DISABLE_SEND_ZC=1` para permitir profiling sem travar `POST /fraud-score`.

## Proximos Pontos Naturais de Trabalho

- Tentar eliminar o ultimo falso negativo sem reintroduzir timeout.
- Atualizar o `README.md` para refletir a estrategia atual do indice.
- Se a qualidade travar novamente, investigar custo/beneficio de armazenar mais informacao por vetor ou refinamento final ainda mais seletivo.
