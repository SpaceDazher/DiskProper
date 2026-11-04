# Пересчёт реестра docs/defects.md по последней колонке строк таблиц §3.1-§3.4.
# Запуск: python3 tmp/x3_recount.py   (из корня репозитория)
#
# Копия логики tmp/w3_recount.py: разбор ограничен четырьмя таблицами реестра
# (от «### 3.1 P0» до «### 3.5»), а не всем документом. Строки мини-таблиц
# разборов проходов (§3.10, §3.16, §3.20 и далее) не считаются: строка реестра —
# только строка таблицы §3.1-§3.4. Статус — по последней колонке строки.
import re
lines = open('docs/defects.md', encoding='utf-8').read().split('\n')
sec, rows = None, []
for line in lines:
    if re.match(r'^### 3\.5 ', line):
        break  # конец таблиц реестра: далее — разборы проходов
    m = re.match(r'^### (3\.\d) (P\d)', line)
    if m:
        sec = m.group(2)
    if sec and line.startswith('| ') and line.count('|') >= 7:
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        rid = cells[0].replace('*', '').strip()
        if re.match(r'^D-\d+$', rid):
            rows.append((sec, rid, cells[-1]))


def status(cell):
    t = cell.replace('*', '').strip().lower()
    for key in ('закрыт', 'частично', 'открыт'):
        if t.startswith(key):
            return key
    return '??'


from collections import Counter
c = Counter()
st = {}
for sec, rid, cell in rows:
    st[rid] = status(cell)
    c[(sec, status(cell))] += 1
print('строк всего:', len(rows))
for key in sorted(c):
    print(' ', key, c[key])
print('закрыто', sum(1 for v in st.values() if v == 'закрыт'),
      '| частично', sum(1 for v in st.values() if v == 'частично'),
      '| открыто', sum(1 for v in st.values() if v == 'открыт'),
      '| не разобрано', sum(1 for v in st.values() if v == '??'))
