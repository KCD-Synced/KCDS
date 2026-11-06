'use strict';

// A woman in a men's top. The game's torso clothing hides a woman's body and under-dress
// (hiding groups FemaleBody and FemaleUnderwear), and her dresses cover the legs themselves, but a
// men's tunic ends at its hem: below it she has no legs. So she wears hose under such a top, as a
// worn-only piece (Player.wearItem): her inventory does not change, and the hose goes when she
// wears something else that covers her legs, takes the top off or is no longer a woman.

// The hose she gets.
const COVER_ITEM = 'HoseJoined01_m01_C';
// The game's men's pieces for the torso, by their item names.
const MEN_TOPS =
  /^(TunicShort|TunicLong|Caftan|Coat|GambesonLong|GambesonShort|Waffenrock|Cuirass|Brigandine|MailLong|MailShort|TorsoPainter)/;
// What covers the legs: hose and leg armour, and a woman's own dresses.
const LEG_COVERS =
  /^(HoseSeparate|HoseJoined|HoseLoose|LegsPadded|LegsPlate|LegsBrigandine|Habit|F_SimpleDress|F_Cotehardie|F_Surcote)/;

function worn(rows, pattern) {
  return rows.some((row) => row.equipped && typeof row.name === 'string' && pattern.test(row.name));
}

// Whether a body of this gender wearing these inventory rows needs the hose.
function needsCover(gender, rows) {
  return gender === 'female' && worn(rows, MEN_TOPS) && !worn(rows, LEG_COVERS);
}

module.exports = { COVER_ITEM, needsCover };
