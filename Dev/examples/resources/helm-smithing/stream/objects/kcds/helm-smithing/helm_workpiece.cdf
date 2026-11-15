<?xml version="1.0" encoding="us-ascii"?>
<!-- The helmet workpiece: the game's axe workpiece skeleton, with the game's half-made helm prop on
     it in place of the axe. The anvil needs a character (it takes its attachments); "Axe" is the
     attachment the hammer strikes (the workpiece's RaycastAttachment) and "hot_01" the one the
     game shows while the piece glows, here without a model of its own. -->
<CharacterDefinition>
  <Model File="objects/characters/assets/blacksmithing/blacksmithing_axe.chr" />
  <AttachmentList>
    <Attachment Type="CA_BONE" AName="Axe" Binding="objects/manmade/task_specific_props/metal_industry/smithing/semifinished_helm.cgf" RelRotation="1,0,0,0" RelPosition="0,0,0" BoneName="particles_01" Flags="0" />
    <Attachment Type="CA_BONE" AName="hot_01" RelRotation="1,0,0,0" RelPosition="0,0,0" BoneName="particles_01" Flags="0" />
    <Attachment Type="CA_BONE" AName="particles_01" RelRotation="1,0,0,0" RelPosition="0,0,0" BoneName="particles_01" Flags="0" />
  </AttachmentList>
</CharacterDefinition>
