<?xml version="1.0" encoding="us-ascii"?>
<!-- The helmet workpiece: the game's axe workpiece skeleton, with the helmet as its "Axe" skin (the
     one the hammer strikes and shapes) and "hot_01" (the same, glowing while hot). The two .skin
     files are made from the player's own game by Dev/tools/assets/workpieces/make_workpiece.py. -->
<CharacterDefinition>
  <Model File="objects/characters/assets/blacksmithing/blacksmithing_axe.chr" Material="objects/kcds/helm-smithing/helm_workpiece.mtl" />
  <AttachmentList>
    <Attachment Type="CA_SKIN" AName="Axe" Binding="objects/kcds/helm-smithing/helm_workpiece.skin" Material="objects/kcds/helm-smithing/helm_workpiece.mtl" Flags="8" />
    <Attachment Type="CA_SKIN" AName="hot_01" Binding="objects/kcds/helm-smithing/helm_workpiece_hot.skin" Material="objects/kcds/helm-smithing/helm_workpiece.mtl" Flags="8" />
    <Attachment Type="CA_BONE" AName="particles_01" RelRotation="1,0,0,0" RelPosition="0,0,0" BoneName="particles_01" Flags="0" />
  </AttachmentList>
</CharacterDefinition>
