# Aplica os patches do CUI, em ordem, sempre a partir da árvore limpa do commit fixado (roda no diretório de fontes do CUI).
# Antes cada patch era testado com "git apply -R --check"; isso falha quando um patch posterior mexe no contexto de um anterior
# (cui-edit-command depois de cui-set-cursor), e a reaplicação quebrava o build. Restaurar e reaplicar tudo é determinístico.
execute_process(COMMAND git checkout -- . RESULT_VARIABLE rc0)
if(NOT rc0 EQUAL 0)
  message(FATAL_ERROR "nao foi possivel restaurar as fontes do CUI antes dos patches")
endif()
foreach(name cui-render-override cui-set-cursor cui-edit-command cui-popover-maxh cui-wheel-take cui-dialog-side cui-emoji-fit cui-popover-offset)
  set(P "${CMAKE_CURRENT_LIST_DIR}/${name}.patch")
  execute_process(COMMAND git apply --ignore-whitespace "${P}" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "falha ao aplicar ${name}.patch")
  endif()
  message(STATUS "patch ${name} aplicado")
endforeach()
