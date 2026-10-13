# Positivo C4128A-14 — solução de áudio ES8326

Contribuição de um usuário de notebook Positivo C4128A-14 usando Arch Linux. **O áudio pelos alto-falantes internos está funcionando normalmente com esta solução**, segundo a confirmação do proprietário. **Microfone ainda não testado.** Fones, HDMI e suspensão/retomada não foram validados.

A solução ajusta mixer ALSA e perfil ACP/WirePlumber para esse hardware. O driver ES8326 já existe no kernel. A causa exata da incompatibilidade UCM ainda precisa de diagnóstico; este projeto não declara suporte universal nem sucesso em todas as distribuições ou modelos Positivo.

## Sintoma observado em outras distribuições

Segundo o relato do proprietário, em algumas distribuições o áudio funciona inicialmente por aproximadamente **10–15 minutos** e depois desaparece, retornando somente após reiniciar o computador. As distribuições e versões específicas ainda precisam ser registradas.

Se outro usuário de Positivo C4128A-14 com codec ES8326 encontrar esse comportamento, esta solução pode ser uma candidata para testar após conferir o hardware e preparar backup. A reprodução pelos alto-falantes com a solução foi confirmada no computador do proprietário; **ainda não foi verificado que ela elimina a perda de áudio após 10–15 minutos em outras distribuições**, nem estabelecida a causa dessa falha. O sintoma temporal também não comprova que a causa seja a mesma incompatibilidade UCM investigada.

## Uso pela comunidade

Código disponibilizado sob **BSD-3-Clause**: a comunidade pode usar, estudar, adaptar e redistribuir, respeitando a licença e os avisos. Mantenedores podem aproveitar a solução para desenvolver uma correção upstream. Não é um lançamento oficial da Positivo.

## Conteúdo

- src/: três scripts auxiliares de áudio, copiados da solução local.
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

Validação local: 15 testes passaram; fontes originais mantiveram os hashes. Os testes verificam lógica e rollback com mocks. A confirmação física cobre somente alto-falantes do computador original.

## Contribuir upstream

Destino inicial sugerido: https://github.com/alsa-project/alsa-ucm-conf/issues

Revise ISSUE-DRAFT.md, complemente evidências do mixer/UCM e identifique quais testes físicos foram realizados. Para uma correção UCM, os mantenedores poderão orientar um PR e validação. Se houver necessidade de alteração do kernel, siga https://www.kernel.org/doc/html/latest/process/submitting-patches.html.

Nada deste pacote foi publicado automaticamente. O repositório privado de dotfiles permanece separado.
