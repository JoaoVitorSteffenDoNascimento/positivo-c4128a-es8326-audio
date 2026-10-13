# Positivo C4128A-14 — solução de áudio ES8326

Contribuição de um usuário de notebook Positivo C4128A-14 usando Arch Linux. **O áudio pelos alto-falantes internos está funcionando normalmente com esta solução**, segundo a confirmação do proprietário. **Microfone ainda não testado.** Fones, HDMI e suspensão/retomada não foram validados.

A solução ajusta mixer ALSA e perfil ACP/WirePlumber para esse hardware. O driver ES8326 já existe no kernel. A causa exata da incompatibilidade UCM ainda precisa de diagnóstico; este projeto não declara suporte universal nem sucesso em todas as distribuições ou modelos Positivo.

## Sintoma observado em outras distribuições

Segundo o relato do proprietário, nos testes do proprietário com **Zorin OS e Linux Mint**, o áudio funciona inicialmente por aproximadamente **10–15 minutos** e depois desaparece, retornando somente após reiniciar o computador. As versões dessas distribuições e dos kernels usados ainda não foram informadas.

Se outro usuário de Positivo C4128A-14 com codec ES8326 encontrar esse comportamento, esta solução pode ser uma candidata para testar após conferir o hardware e preparar backup. A reprodução pelos alto-falantes com a solução foi confirmada no computador do proprietário; **ainda não foi verificado que ela elimina a perda de áudio após 10–15 minutos em outras distribuições**, nem estabelecida a causa dessa falha. O sintoma temporal também não comprova que a causa seja a mesma incompatibilidade UCM investigada.

## Uso pela comunidade

Código disponibilizado sob **BSD-3-Clause**: a comunidade pode usar, estudar, adaptar e redistribuir, respeitando a licença e os avisos. Mantenedores podem aproveitar a solução para desenvolver uma correção upstream. Não é um lançamento oficial da Positivo.

## Conteúdo

- src/: scripts Python originais e implementação C nativa do mixer, ativação e desfazer.
- config/: quatro templates de configuração; @HOME@ deve ser substituído pelo HOME real.
- tests/: testes com mixer simulado, sem mudanças no áudio real.
- ISSUE-DRAFT.md: relato técnico em inglês para revisão e envio ao ALSA.
- SOURCE-VERIFICATION.json: hashes das fontes locais para verificar que permaneceram intactas.

Não há instalador automático. Não copie templates diretamente para /etc ou ~/.config: a integração requer revisão dos caminhos, compatibilidade do hardware e backup antes de alterar arquivos. Os scripts podem mudar mixer e reiniciar serviços quando executados com ações de ativação; os testes apenas simulam essas operações.

O helper de desfazer depende de manifestos de backup da instalação original. Eles não são incluídos aqui; não execute desfazer sem preparar um backup compatível. A cópia em src muda a localização ROOT dos relatórios, portanto precisa de revisão antes de instalação em outra máquina. Esta distribuição de código não é uma instalação validada em outro computador.

## Testes isolados

```sh
PYTHONPATH=src python3 -m unittest discover -s tests -v
```

Validação local: 18 testes Python/paridade e duas suítes C passaram; fontes originais mantiveram os hashes. Os testes verificam lógica e rollback com mocks. A confirmação física cobre somente alto-falantes do computador original.

## Versão C e correspondência com Python

`src/audio_interno.c`, `src/audio_mixer.c` e `src/audio_alsa.c` portam as ações **inicializar, aplicar e desfazer** do script principal. Usam libasound, json-c e libcrypto, sem iniciar um interpretador Python. Os templates de configuração continuam compartilhados pelas duas versões.

A lógica de inicialização mantém o mesmo plano: Headphone Switch desligado, Speaker Switch ligado, DAC obrigatório, demais volumes digitais opcionais, máximo de até 0 dB calculado pelo driver, backup antes de escrever, verificação de leitura e rollback em falha. A ativação mantém os comandos systemctl/pactl, perfil, porta, volume de 40%, retomada de streams e tom curto de teste. Desfazer usa a allowlist de quatro configurações e seus hashes antes de restaurar o manifesto original.

Por segurança, o C exige uma ação explícita: sem argumentos, mostra ajuda. Relatórios ficam por padrão em `${XDG_STATE_HOME:-$HOME/.local/state}/positivo-es8326`, com permissões privadas. Para usar os mesmos backups existentes da versão Python, informe `--state-dir` apontando para a pasta que contém `backup-audio-interno.txt` e os relatórios. Não distribua esse diretório de diagnósticos com o código.

O C porta os fluxos funcionais, não é uma cópia byte a byte dos logs: timestamps, mensagens e pasta padrão diferem. O diagnóstico interativo de conector do helper legado permanece disponível em Python. Nenhuma versão instala automaticamente os templates nem cria os manifestos de instalação necessários ao desfazer.

### Compilar e verificar

Precisa de compilador C, make, pkg-config e arquivos de desenvolvimento de ALSA, json-c e OpenSSL/libcrypto. No Arch, as bibliotecas usadas já estavam disponíveis; outras distribuições podem separar cabeçalhos em pacotes de desenvolvimento.

```sh
make
make check
./build/audio_interno --help
# Apenas lê hardware/mixer e mostra o plano; não altera áudio:
./build/audio_interno inicializar --dry-run
```

`aplicar`, `inicializar` sem dry-run e `desfazer` alteram estado: somente execute após revisar compatibilidade e backups. Não substitua um setup funcional automaticamente.

### Resultado dos testes e limites

- Compilação C11 com -Wall -Wextra -Werror -Wformat=2 concluída.
- Suíte C de mixer: canais, sensores preservados, DAC obrigatório, controles opcionais, falhas de dB, backup anterior, idempotência, verificação e rollback.
- Suíte C de helpers: argumentos sem shell, timeout/saída de processos, seleção de perfis e undo de arquivos temporários com conflito e modos preservados.
- 18 testes Python/paridade: o WAV C é idêntico ao Python amostra por amostra e o plano do mixer coincide nos fixtures estéreo/com controle opcional ausente.
- AddressSanitizer e UndefinedBehaviorSanitizer passaram nos testes C. LeakSanitizer não está disponível neste ambiente sob ptrace; os testes usaram ASAN_OPTIONS=detect_leaks=0, portanto não afirmamos validação de vazamentos.
- A versão C **não foi aplicada nem validada fisicamente** no codec. A tentativa read-only de dry-run encontrou a placa inacessível neste ambiente. O áudio funcional confirmado pelo proprietário continua usando a instalação original Python.

Não há benchmark demonstrando vantagem prática de C aqui. O Python já usa libasound em C por ctypes; traduzir o programa não comprova a resolução do sintoma de 10–15 minutos ou de falhas de driver/firmware. Microfone, fones e estabilidade prolongada continuam pendentes.

Referência da API usada: https://www.alsa-project.org/alsa-doc/alsa-lib/group___control.html

## Contribuir upstream

Destino inicial sugerido: https://github.com/alsa-project/alsa-ucm-conf/issues

Revise ISSUE-DRAFT.md, complemente evidências do mixer/UCM e identifique quais testes físicos foram realizados. Para uma correção UCM, os mantenedores poderão orientar um PR e validação. Se houver necessidade de alteração do kernel, siga https://www.kernel.org/doc/html/latest/process/submitting-patches.html.

Nada deste pacote foi publicado automaticamente. O repositório privado de dotfiles permanece separado.
