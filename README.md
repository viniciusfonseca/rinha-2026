# Rinha de Backend 2026 em C

Implementação em C com:

- API HTTP em `io_uring`
- load balancer TCP round-robin em `io_uring`
- vetorização de 14 dimensões conforme a especificação oficial
- índice vetorial compacto gerado a partir de `references.json.gz`

## Arquitetura

- `lb`: recebe tráfego na porta `9999` e distribui conexões entre `api1` e `api2`
- `api1` / `api2`: expõem `GET /ready` e `POST /fraud-score`
- `preprocess`: utilitário de build que converte o dataset oficial em um índice binário local

## Estratégia de busca

O dataset oficial é transformado, no build, em:

- vetores quantizados de 14 dimensões
- labels binários
- quatro tabelas LSH para reduzir o conjunto de candidatos por consulta

Na consulta:

1. o payload é vetorizado conforme `REGRAS_DE_DETECCAO.md`
2. a busca usa LSH para coletar candidatos
3. a distância euclidiana é calculada apenas nesses candidatos
4. os 5 vizinhos mais próximos determinam `fraud_score`

## Como rodar

```bash
docker compose up --build
```

A imagem baixa `references.json.gz` do repositório oficial durante o build e gera `/opt/rinha/index.bin`.

## Observação

Para uma submissão oficial da Rinha, vale publicar a imagem em um registry público e ajustar o `docker-compose.yml` para apontar para essa imagem em vez de depender de `build`.
