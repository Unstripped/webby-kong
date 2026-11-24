import { useEffect, useState } from 'preact/hooks';
import { api } from '../../shared/webauthn';
import { fail } from '../fail';
import { fmt, fmtPlay } from '../format';
import type { PanelProps, Player } from '../types';

const DASH = '\u2013';
const HEADS = ['Player', 'Save', 'Progress', 'Golden bananas', 'Keys', 'Play time', 'Last played'];

// Every player's saves, fetched once at mount (no refresh).
export function PlayersPanel({ me }: PanelProps) {
  const [players, setPlayers] = useState<Player[] | null>(null);
  useEffect(() => {
    api<{ players: Player[] }>('GET', '/api/players')
      .then((r) => setPlayers(r.players))
      .catch(fail);
  }, []);

  const who = (u: Player) => (
    <td>
      {u.username}
      {' '}
      {u.owner ? <span class="badge owner">owner</span> : null}
      {' '}
      {u.id === me.user.id ? <span class="badge">you</span> : null}
      {' '}
      {u.disabled ? <span class="badge bad">disabled</span> : null}
    </td>
  );

  return (
    <div>
      {players && (
        <div class="tbl-wrap">
          <table class="tbl">
            <thead>
              <tr>
                {HEADS.map((t) => (
                  <th key={t}>{t}</th>
                ))}
              </tr>
            </thead>
            <tbody>
              {players.flatMap((u) => {
                if (!u.saves.length) {
                  return [
                    <tr key={u.id}>
                      {who(u)}
                      <td class="muted">No saves yet</td>
                      <td>{DASH}</td>
                      <td>{DASH}</td>
                      <td>{DASH}</td>
                      <td>{DASH}</td>
                      <td>{DASH}</td>
                    </tr>,
                  ];
                }
                return u.saves.map((s) => {
                  const m = s.summary;
                  const cells = m
                    ? [m.percent + '%', String(m.goldenBananas), m.keys + '/8', fmtPlay(m.playSeconds)]
                    : [DASH, DASH, DASH, DASH];
                  return (
                    <tr key={u.id + '/' + s.id}>
                      {who(u)}
                      <td>{s.name}</td>
                      {cells.map((t, i) => (
                        <td key={i}>{t}</td>
                      ))}
                      <td>{fmt(s.played_at)}</td>
                    </tr>
                  );
                });
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
