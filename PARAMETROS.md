# Parâmetros do Índice

Este arquivo resume os parâmetros principais do índice IVF e da quantização usada neste projeto.

## IVF

### `RINHA_IVF_NLIST = 1024`

Quantidade de listas invertidas do índice.

- Mais listas deixam cada lista menor.
- Isso tende a acelerar a busca, porque cada consulta percorre menos vetores por lista.
- Em troca, o índice fica mais sensível a escolhas ruins de listas candidatas.

Na prática, `NLIST` controla a granularidade do particionamento.

### `RINHA_IVF_NPROBE = 4`

Quantidade de listas que a consulta realmente examina.

- Aumentar `NPROBE` melhora a precisão da busca, porque a query olha para mais regiões do índice.
- Diminuir `NPROBE` melhora a velocidade, mas pode piorar o recall.

Esse é o principal botão de troca entre velocidade e qualidade em runtime.

### `RINHA_IVF_TRAIN_SAMPLES = 131072`

Quantidade de amostras usada para treinar os centróides do IVF.

- Mais amostras produzem centróides mais representativos.
- Isso pode melhorar a qualidade da partição e, por consequência, a precisão da busca.
- O custo extra aparece no preprocessamento, não na consulta online.

### `RINHA_IVF_KMEANS_ITERS = 16`

Número de iterações do `k-means` no treino do índice.

- Mais iterações refinam melhor os centróides.
- Isso pode melhorar um pouco a qualidade da busca.
- O custo também fica no build, não no runtime.

## Quantização

### `RINHA_VECTOR_QUANT_SCALE = 65534`

Escala usada para mapear valores `float` para `uint16_t`.

- Quanto maior a escala, menor o erro de arredondamento.
- Como este projeto usa `16 bits`, a escala já está praticamente no limite útil.
- O valor `65535` fica reservado para sentinela.

### `RINHA_VECTOR_QUANT_MISSING = 65535`

Sentinela para representar valor ausente.

- Quando um valor é inexistente, inválido ou fora do intervalo esperado, ele pode cair nessa sentinela.
- Na dequantização, a sentinela volta como `-1.0f`.
- Isso evita confundir dado ausente com dado válido.

## Efeito Prático

- `NPROBE` é o parâmetro que mais pesa em velocidade versus precisão durante a consulta.
- `NLIST` define o tamanho médio das listas e a granularidade do índice.
- `TRAIN_SAMPLES` e `KMEANS_ITERS` afetam principalmente a qualidade do índice gerado no build.
- A quantização em `16 bits` reduz o erro de representação sem explodir o tamanho do índice.

## Resumo Rápido

- Quer mais velocidade: reduza `NPROBE`.
- Quer mais precisão: aumente `NPROBE`.
- Quer melhor partição: aumente `TRAIN_SAMPLES` ou `KMEANS_ITERS`.
- Quer menos erro numérico: mantenha a quantização em `16 bits`.
